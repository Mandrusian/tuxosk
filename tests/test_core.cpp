// Core tests (no Wayland): swipe accuracy on synthetic noisy paths, typing/autocorrect/UI flows.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>

#include "../src/engine.h"
#include "../src/layout.h"
#include "../src/settings.h"
#include "../src/ui.h"

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
        std::mt19937 rng5(5), rng6(6), rng7(7);
        auto path = synth(L, "hello", rng5, 0.05f);
        { auto r = dec.decode(path, 4); for (auto &x : r) std::printf("  cand %s %.3f\n", x.first.c_str(), x.second); }
        e.swipe(path);
        std::printf("swipe1 [%s]\n", m.view().c_str());
        CHECK(m.view() == "Hello ");                      // auto-cap, auto space
        e.swipe(synth(L, "world", rng6, 0.05f));
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
        e2.swipe(synth(L, "good", rng7, 0.05f));
        e2.backspace();
        CHECK(m2.view().empty());                         // backspace after a swipe removes the whole word
    }
    {
        Mock m; m.im = false; Engine e(lex, dec, m); e.activated();
        e.typeChar("l"); e.typeChar("s"); e.enter();
        CHECK(m.keys.size() == 3);                        // no input method: plain key events
    }

    // contraction suggestions & literal selection (note: auto-cap at field start capitalises first letter)
    {
        Mock m; Engine e(lex, dec, m); e.activated();
        for (char c : std::string("dont")) e.typeChar(std::string(1, c));
        CHECK(e.suggestions().size() >= 2);
        CHECK(e.suggestions()[0] == "Dont");
        CHECK(e.suggestions()[1] == "Don't");
        CHECK(e.mainSuggestion() == 1);
        e.pickSuggestion(0);                              // pick literal "Dont"
        CHECK(m.view() == "Dont ");
    }

    // shift states, noPredict, charToKey, layout helpers
    {
        Mock m; Engine e(lex, dec, m); e.activated();
        CHECK(e.shift() == Engine::Once);                 // auto-cap at start of field
        e.tapShift();
        CHECK(e.shift() == Engine::Off);
        e.doubleTapShift();
        CHECK(e.shift() == Engine::Lock);
        e.typeChar("a"); e.typeChar("b");
        e.space();
        CHECK(m.view() == "AB ");
        e.setNoPredict(true);
        CHECK(e.suggestions().empty());
        e.typeChar("c");
        CHECK(m.view() == "AB C");

        uint32_t code = 0, mods = 0;
        CHECK(Engine::charToKey("a", code, mods) && code == 30 && mods == 0);
        CHECK(Engine::charToKey("A", code, mods) && code == 30 && mods == 1);
        CHECK(Engine::charToKey("?", code, mods) && code == 53 && mods == 1);
        CHECK(!Engine::charToKey("€", code, mods));
        CHECK(Layout::neighbour('q', 'w'));
        CHECK(!Layout::neighbour('q', 'm'));
    }

    // blocklist (case-insensitive + contractions) & user dictionary persistence
    {
        namespace fs = std::filesystem;
        const fs::path tmpDir = fs::temp_directory_path() / "tuxosk_test_tmp";
        fs::create_directories(tmpDir);
        const fs::path blk = tmpDir / "block.txt";
        const fs::path usr = tmpDir / "user.txt";
        {
            std::ofstream out(blk);
            out << "HELLO\n" << "don't\n";
        }
        Lexicon filtered;
        filtered.loadBlocklist(blk.string());
        CHECK(filtered.load(dict));
        CHECK(!filtered.known("hello"));
        CHECK(!filtered.known("don't"));
        CHECK(!filtered.known("dont"));

        filtered.addUser("Zaphod");
        filtered.addUser("qwertyuiopx");
        filtered.saveUser(usr.string());

        Lexicon reloaded;
        CHECK(reloaded.load(dict));
        CHECK(reloaded.loadUser(usr.string()));
        CHECK(reloaded.known("qwertyuiopx"));

        // settings load/save roundtrip
        const fs::path cfgPath = tmpDir / "config";
        Settings s1;
        s1.lang = "de";
        s1.autocorrect = false;
        s1.light = true;
        s1.tall = true;
        s1.reduceMotion = true;
        s1.filters["mywords"] = false;
        s1.save(cfgPath.string());

        Settings s2;
        s2.load(cfgPath.string());
        CHECK(s2.lang == "de");
        CHECK(!s2.autocorrect);
        CHECK(s2.light);
        CHECK(s2.tall);
        CHECK(s2.reduceMotion);
        CHECK(s2.filters.count("slurs") && s2.filters["slurs"]);
        CHECK(s2.filters.count("mywords") && !s2.filters["mywords"]);

        fs::remove_all(tmpDir);
    }

    // Ui interaction, timers lifecycle, modal touch handling, capture mode, touchCancelAll
    {
        struct MockTimers : Timers {
            int nextId = 1;
            std::map<int, std::function<bool()>> active;
            int add(int, std::function<bool()> cb) override {
                int id = nextId++;
                active[id] = std::move(cb);
                return id;
            }
            void remove(int id) override { active.erase(id); }
            void fireAll() {
                std::vector<int> ids;
                for (auto &kv : active) ids.push_back(kv.first);
                for (int id : ids) {
                    auto it = active.find(id);
                    if (it == active.end()) continue;
                    auto cb = it->second;
                    if (!cb()) active.erase(id);
                }
            }
        } timers;

        Mock m;
        {
            Engine e(lex, dec, m);
            Settings st;
            Ui ui(e, lex, dec, timers);
            ui.setSettings(&st);
            ui.setLanguages({{"en", "English (US)", "QWERTY"}});
            ui.setFilters({{"slurs", "Slurs"}});
            ui.resize(540, 1);
            e.activated();

            // tap a key via keyPos
            float x = 0, y = 0;
            CHECK(ui.keyPos("H", x, y));
            ui.touchDown(1, x, y);
            ui.touchUp(1);
            CHECK(m.view() == "H");

            // open modal while a touch is down (space long-press simulation) and verify cleanup
            CHECK(ui.keyPos("space", x, y));
            ui.touchDown(2, x, y);
            CHECK(!timers.active.empty());
            timers.fireAll(); // triggers space long-press -> openModal(Modal::Langs)
            CHECK(ui.modalOpen());
            ui.closeModalNow();
            CHECK(!ui.modalOpen());

            // capture mode: tapping hidden strip suggestion should not commit, Enter saves blocked word
            std::string blocked;
            ui.onBlockWord = [&](const std::string &w) { blocked = w; };
            ui.openFilters();
            // tap "Block a word…" row near bottom of Filters dialog
            ui.modalTouchDown(270, Layout::heightFor(540) / 2 + 40);
            ui.modalTouchUp();
            CHECK(!ui.modalOpen());
            // tap strip slot 0 (should be ignored in capture mode)
            ui.touchDown(3, 80, 20);
            ui.touchUp(3);
            CHECK(m.view() == "H");
            // type "bad" and press enter
            CHECK(ui.keyPos("b", x, y)); ui.touchDown(4, x, y); ui.touchUp(4);
            CHECK(ui.keyPos("a", x, y)); ui.touchDown(5, x, y); ui.touchUp(5);
            CHECK(ui.keyPos("d", x, y)); ui.touchDown(6, x, y); ui.touchUp(6);
            CHECK(ui.keyPos("enter", x, y)); ui.touchDown(7, x, y); ui.touchUp(7);
            CHECK(blocked == "bad");

            // touchCancelAll with high touch ID (>9) clears timers cleanly
            CHECK(ui.keyPos("e", x, y));
            ui.touchDown(42, x, y);
            ui.touchCancelAll();
        }
        // ~Ui() must clean up all remaining timers (including animation fade timers)
        CHECK(timers.active.empty());
    }
    std::printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
