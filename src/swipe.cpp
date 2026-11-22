#include "swipe.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kEnd = 0.30f;   // extra cost for starting/ending away from the first/last letter

inline float dist(const Pt &a, const Pt &b) { return std::hypot(a.x - b.x, a.y - b.y); }

float polyLen(const std::vector<Pt> &p) {
    float l = 0;
    for (size_t i = 1; i < p.size(); ++i) l += dist(p[i - 1], p[i]);
    return l;
}

// N equally spaced points along a polyline (first and last point included)
template <size_t N>
void resample(const std::vector<Pt> &p, std::array<Pt, N> &out) {
    const float total = polyLen(p);
    if (p.size() < 2 || total <= 1e-3f) { out.fill(p.empty() ? Pt{} : p[0]); return; }
    const float step = total / (N - 1);
    out[0] = p[0];
    size_t seg = 1; float acc = 0;                      // acc = length up to p[seg-1]
    for (size_t k = 1; k < N; ++k) {
        const float target = step * k;
        while (seg < p.size() - 1 && acc + dist(p[seg - 1], p[seg]) < target) { acc += dist(p[seg - 1], p[seg]); ++seg; }
        const float sl = dist(p[seg - 1], p[seg]);
        const float t = sl > 1e-6f ? std::min(1.f, std::max(0.f, (target - acc) / sl)) : 0.f;
        out[k] = {p[seg - 1].x + (p[seg].x - p[seg - 1].x) * t, p[seg - 1].y + (p[seg].y - p[seg - 1].y) * t};
    }
    out[N - 1] = p.back();
}

float segDist(const Pt &p, const Pt &a, const Pt &b) {
    const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float t = l2 > 1e-6f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0.f;
    t = std::min(1.f, std::max(0.f, t));
    return std::hypot(p.x - (a.x + dx * t), p.y - (a.y + dy * t));
}

}  // namespace

void SwipeDecoder::setLayout(const std::array<Pt, 26> &centres, float keyW, float keyH) {
    c_ = centres; kw_ = keyW; kh_ = keyH;
    build();
}

void SwipeDecoder::setLexicon(const Lexicon *lex) { lex_ = lex; build(); }

void SwipeDecoder::build() {
    tpl_.clear();
    if (!lex_) return;
    const auto &ws = lex_->words();
    tpl_.reserve(ws.size());
    for (int i = 0; i < (int)ws.size(); ++i) {
        const std::string &k = ws[i].key;
        if (k.size() < 2) continue;
        std::vector<Pt> line;
        int prev = -1; bool ok = true;
        for (char ch : k) {
            if (ch < 'a' || ch > 'z') { ok = false; break; }
            const int l = ch - 'a';
            if (l == prev) continue;               // doubled letters are one visit
            prev = l; line.push_back(c_[l]);
        }
        if (!ok || line.size() < 2) continue;
        Tpl t;
        t.word = i; t.first = k.front() - 'a'; t.last = k.back() - 'a';
        t.len = polyLen(line);
        resample<N>(line, t.pts);
        tpl_.push_back(t);
    }
}

std::vector<std::pair<std::string, float>> SwipeDecoder::decode(const std::vector<Pt> &raw, int n) const {
    std::vector<std::pair<std::string, float>> res;
    if (!lex_ || tpl_.empty() || raw.size() < 2) return res;
    std::vector<Pt> path;
    for (const Pt &p : raw) if (path.empty() || dist(path.back(), p) >= 1.5f) path.push_back(p);
    const float plen = polyLen(path);
    if (path.size() < 2 || plen < 0.8f * kw_) return res;
    std::array<Pt, N> P;
    resample<N>(path, P);

    const float endR = 1.9f * kw_;
    struct Cand { int t; float cost; };
    std::vector<Cand> cand;
    cand.reserve(2048);
    for (int ti = 0; ti < (int)tpl_.size(); ++ti) {
        const Tpl &t = tpl_[ti];
        if (dist(P[0], c_[t.first]) > endR || dist(P[N - 1], c_[t.last]) > endR) continue;
        const float ratio = plen / std::max(1.f, t.len);
        if (ratio < 0.45f || ratio > 2.3f) continue;
        float s = 0, ws = 0;
        for (int i = 0; i < N; ++i) {
            const float w = (i == 0 || i == N - 1) ? 2.f : 1.f;
            s += w * dist(P[i], t.pts[i]); ws += w;
        }
        const float d0 = dist(P[0], c_[t.first]) / kw_, d1 = dist(P[N - 1], c_[t.last]) / kw_;
        cand.push_back({ti, s / ws / kw_ + kEnd * (d0 * d0 + d1 * d1)});
    }
    if (cand.empty()) return res;
    const int keep = std::min<int>(80, (int)cand.size());
    std::partial_sort(cand.begin(), cand.begin() + keep, cand.end(), [](const Cand &a, const Cand &b) { return a.cost < b.cost; });
    cand.resize(keep);

    const auto &ws = lex_->words();
    std::vector<std::pair<std::string, float>> scored;
    for (const Cand &c : cand) {
        const Word &w = ws[tpl_[c.t].word];
        // coverage: every distinct letter of the word should be passed close by
        float cover = 0; int prev = -1;
        for (char ch : w.key) {
            const int l = ch - 'a';
            if (l == prev) continue;
            prev = l;
            float d = 1e9f;
            for (size_t i = 1; i < path.size(); ++i) d = std::min(d, segDist(c_[l], path[i - 1], path[i]));
            const float over = d / kw_ - 0.55f;
            if (over > 0) cover += over * over;
        }
        const float total = c.cost + 0.55f * cover - 0.30f * w.f;
        scored.emplace_back(w.out, total);
    }
    std::sort(scored.begin(), scored.end(), [](auto &a, auto &b) { return a.second < b.second; });
    for (auto &s : scored) {
        bool dup = false;
        for (auto &r : res) if (r.first == s.first) { dup = true; break; }
        if (!dup) res.push_back(s);
        if ((int)res.size() >= n) break;
    }
    return res;
}
