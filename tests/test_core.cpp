// Core tests (no Wayland): swipe accuracy on synthetic noisy paths, typing/autocorrect flows.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "../src/engine.h"
#include "../src/layout.h"

struct Mock : Output {
    std::string text;           // committed text
    std::string pre;
    std::vector<uint32_t> keys;
    bool im = true;
    void commit(const std::string &s) override { text += s; pre.clear(); }
    void preedit(const std::string &s) override { pre = s; }
    void del(int b, int) override { text.erase(text.size() - std::min<size_t>(b, text.size())); }
    void key(uint32_t k, uint32_t) override {
        keys.push_back(k);
        if (k == K_BACKSPACE && !text.empty()) text.pop_back();
        if (k == K_ENTER) text += "\n";
    }
    void flush() override {}
    bool imActive() const override { return im; }
    std::string view() const { return text + pre; }
};

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

// path through the key centres of `w` with smooth corners and gaussian noise
static std::vector<Pt> synth(const Layout &L, const std::string &w, std::mt19937 &rng, float sigma) {
    std::vector<Pt> ctl;
    int prev = -1;
    for (char c : w) { int l = c - 'a'; if (l < 0 || l > 25 || l == prev) continue; prev = l; ctl.push_back(L.centres()[l]); }
    std::normal_distribution<float> nd(0, sigma * L.keyW());
    std::vector<Pt> out;
    for (size_t i = 0; i + 1 < ctl.size(); ++i) {
        Pt a = ctl[i], b = ctl[i + 1];
        Pt p0 = i ? ctl[i - 1] : a, p3 = i + 2 < ctl.size() ? ctl[i + 2] : b;
        for (int s = 0; s < 12; ++s) {      // Catmull-Rom
            float t = s / 12.f, t2 = t * t, t3 = t2 * t;
            auto cr = [&](float q0, float q1, float q2, float q3) {
                return 0.5f * ((2 * q1) + (-q0 + q2) * t + (2 * q0 - 5 * q1 + 4 * q2 - q3) * t2 + (-q0 + 3 * q1 - 3 * q2 + q3) * t3);
            };
            out.push_back({cr(p0.x, a.x, b.x, p3.x), cr(p0.y, a.y, b.y, p3.y)});
        }
    }
    out.push_back(ctl.back());
    // low-frequency wobble + per-point jitter
    float ox = nd(rng), oy = nd(rng);
    for (size_t i = 0; i < out.size(); ++i) {
        if (i % 8 == 0) { ox = 0.6f * ox + nd(rng); oy = 0.6f * oy + nd(rng); }
        out[i].x += ox; out[i].y += oy;
    }
    return out;
}

int main(int argc, char **argv) {
    const char *dict = argc > 1 ? argv[1] : "data/en.txt";
    Lexicon lex;
    if (!lex.load(dict)) { std::printf("cannot load %s\n", dict); return 2; }
    std::printf("words: %zu\n", lex.words().size());
    Layout L; L.build(Page::Letters, 540, Layout::heightFor(540), {});
    SwipeDecoder dec; dec.setLayout(L.centres(), L.keyW(), L.keyH());
    auto t0 = std::chrono::steady_clock::now();
    dec.setLexicon(&lex);
    auto t1 = std::chrono::steady_clock::now();
    std::printf("template build: %.0f ms\n", std::chrono::duration<double, std::milli>(t1 - t0).count());

    // swipe accuracy over the 3000 most frequent words (length>=3), several noise levels
    for (float sigma : {0.10f, 0.25f, 0.40f}) {
        std::mt19937 rng(1);
        int n = 0, top1 = 0, top3 = 0;
        double ms = 0;
        for (const Word &w : lex.words()) {
            if (n >= 1500) break;
            if (w.key.size() < 3 || w.out != w.key) continue;
            auto path = synth(L, w.key, rng, sigma);
            auto a = std::chrono::steady_clock::now();
            auto r = dec.decode(path, 3);
            ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
            ++n;
            if (!r.empty() && r[0].first == w.out) ++top1;
            for (auto &x : r) if (x.first == w.out) { ++top3; break; }
        }
        std::printf("noise %.2f key: top1 %.1f%%  top3 %.1f%%  decode %.2f ms avg (%d words)\n", sigma, 100.0 * top1 / n, 100.0 * top3 / n, ms / n, n);
        if (sigma <= 0.25f) CHECK(100.0 * top3 / n > 85);
    }

    // typing flows
    {
        Mock m; Engine e(lex, dec, m);
        e.activated();
        auto type = [&](const std::string &s) { for (char c : s) { if (c == ' ') e.space(); else e.typeChar(std::string(1, c)); } };
        type("teh ");
        CHECK(m.view() == "The ");                        // autocorrect + auto-capital at field start
        type("dont ");
        std::printf("dont [%s]\n", m.view().c_str());
        CHECK(m.view() == "The don't ");
        e.backspace();                                    // undo autocorrect -> back to typed word
        CHECK(m.view() == "The dont");
        type("x");
        e.space();
    }
    {
        Mock m; Engine e(lex, dec, m); e.activated();
        auto path = synth(L, "hello", *new std::mt19937(5), 0.05f);
        { auto r = dec.decode(path, 4); for (auto &x : r) std::printf("  cand %s %.3f\n", x.first.c_str(), x.second); }
        e.swipe(path);
        std::printf("swipe1 [%s]\n", m.view().c_str());
        CHECK(m.view() == "Hello ");                      // auto-cap, auto space
        e.swipe(synth(L, "world", *new std::mt19937(6), 0.05f));
        CHECK(m.view() == "Hello world ");
        e.typeChar(".");
        std::printf("after dot [%s]\n", m.view().c_str());
        CHECK(m.view() == "Hello world.");                // auto space removed before punctuation
        e.space();
        CHECK(m.view() == "Hello world. ");
        for (char c : std::string("ok")) e.typeChar(std::string(1, c));
        e.space(); e.space();
        CHECK(m.view() == "Hello world. Ok. ");
        Mock m2; Engine e2(lex, dec, m2); e2.activated();
        e2.swipe(synth(L, "good", *new std::mt19937(7), 0.05f));
        e2.backspace();
        CHECK(m2.view().empty());                         // backspace after a swipe removes the whole word
    }
    {
        Mock m; m.im = false; Engine e(lex, dec, m); e.activated();
        e.typeChar("l"); e.typeChar("s"); e.enter();
        CHECK(m.keys.size() == 3);                        // no input method: plain key events
    }
    std::printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
