#include "lexicon.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace {

// the subtitle word list splits contractions ("don't" -> "don" + "'t"); these stems are not words
const std::set<std::string> kStray = {"don", "didn", "doesn", "isn", "wasn", "aren", "couldn", "wouldn", "shouldn",
                                      "haven", "hasn", "hadn", "weren", "won", "ll", "ve", "re", "ain", "mustn",
                                      "needn", "mightn", "shan"};
const std::set<std::string> kTwo = {"of", "to", "in", "is", "it", "he", "we", "me", "my", "on", "no", "do", "so", "up",
                                    "go", "be", "as", "at", "an", "am", "by", "if", "or", "us", "oh", "ok", "hi", "ya",
                                    "ah", "uh", "um", "yo", "id", "tv", "pm", "dr", "mr", "ms"};

// apostrophe-less spellings that are only typos of a contraction, not words
const std::set<std::string> kTypos = {"dont", "doesnt", "didnt", "cant", "wont", "im", "youre", "theyre", "ive", "thats",
                                      "whats", "hes", "shes", "wasnt", "isnt", "arent", "couldnt", "wouldnt", "shouldnt",
                                      "havent", "hasnt", "hadnt", "werent", "theres", "heres", "whos", "youve", "youll",
                                      "theyll", "weve", "hows", "itll", "thatll", "yall", "maam", "oclock", "id", "ya"};

struct Contraction { const char *out; const char *stem; double mul; };
const Contraction kContractions[] = {
    {"don't", "don", 1}, {"didn't", "didn", 1}, {"doesn't", "doesn", 1}, {"isn't", "isn", 1}, {"wasn't", "wasn", 1},
    {"aren't", "aren", 1}, {"couldn't", "couldn", 1}, {"wouldn't", "wouldn", 1}, {"shouldn't", "shouldn", 1},
    {"haven't", "haven", 1}, {"hasn't", "hasn", 1}, {"hadn't", "hadn", 1}, {"weren't", "weren", 1},
    {"won't", "won", 1}, {"can't", "can", 0.3}, {"i'm", "'m", 1}, {"i'll", "'ll", 0.15}, {"i've", "'ve", 0.15},
    {"i'd", "'d", 0.08}, {"you're", "'re", 0.35}, {"you'll", "'ll", 0.1}, {"you've", "'ve", 0.1},
    {"he's", "'s", 0.06}, {"she's", "'s", 0.04}, {"it's", "'s", 0.3}, {"that's", "'s", 0.15},
    {"there's", "'s", 0.06}, {"what's", "'s", 0.12}, {"let's", "'s", 0.05}, {"we're", "'re", 0.15},
    {"they're", "'re", 0.12}, {"who's", "'s", 0.02}, {"here's", "'s", 0.02}, {"how's", "'s", 0.01},
    {"we'll", "'ll", 0.06}, {"they'll", "'ll", 0.03}, {"we've", "'ve", 0.04}, {"they've", "'ve", 0.02},
    {"he'll", "'ll", 0.03}, {"she'll", "'ll", 0.02}, {"it'll", "'ll", 0.03}, {"that'll", "'ll", 0.01},
    {"y'all", "'ll", 0.01}, {"ma'am", "'m", 0.01}, {"o'clock", "'m", 0.01}};

std::string strip(const std::string &s) {
    std::string r;
    for (char c : s) if (c != '\'') r += c;
    return r;
}

}  // namespace

void Lexicon::loadBlocklist(const std::string &path) {
    std::ifstream in(path);
    std::string w;
    while (std::getline(in, w)) {
        while (!w.empty() && (w.back() == '\r' || w.back() == ' ')) w.pop_back();
        if (!w.empty()) block_.insert(w);
    }
}

bool Lexicon::load(const std::string &path) {
    std::ifstream in(path);
    if (!in) return false;
    std::unordered_map<std::string, double> counts;
    std::vector<std::pair<std::string, double>> order;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string w; double c = 0;
        if (!(ss >> w >> c) || c <= 0) continue;
        for (char &ch : w) ch = (char)std::tolower((unsigned char)ch);
        counts[w] = c;
        order.emplace_back(w, c);
    }
    if (order.empty()) return false;
    for (auto &[w, c] : order) {
        if (w[0] == '\'') continue;
        bool ok = true;
        for (char ch : w) if (!((ch >= 'a' && ch <= 'z') || ch == '\'')) { ok = false; break; }
        if (!ok || kStray.count(w) || kTypos.count(w) || block_.count(w) || block_.count(strip(w))) continue;
        if (w.size() == 1 && w != "a" && w != "i") continue;
        if (w.size() == 2 && !kTwo.count(w)) continue;
        words_.push_back({w, strip(w), (float)c});
    }
    for (auto &k : kContractions) {
        auto it = counts.find(k.stem);
        if (it == counts.end()) continue;
        std::string o = k.out;
        words_.push_back({o, strip(o), (float)(it->second * k.mul)});
    }
    // normalise log frequency to 0..1
    float mx = 0, mn = 1e30f;
    for (auto &w : words_) { mx = std::max(mx, w.f); mn = std::min(mn, w.f); }
    const float lmx = std::log10(mx), lmn = std::log10(std::max(1.f, mn));
    for (auto &w : words_) w.f = (std::log10(std::max(1.f, w.f)) - lmn) / std::max(0.01f, lmx - lmn);
    std::sort(words_.begin(), words_.end(), [](const Word &a, const Word &b) { return a.f > b.f; });
    index();
    return true;
}

void Lexicon::index() {
    byKey_.clear(); byOut_.clear(); alpha_.clear();
    for (int i = 0; i < (int)words_.size(); ++i) {
        byKey_.emplace(words_[i].key, i);   // sorted by freq: first wins
        byOut_.emplace(words_[i].out, i);
        alpha_.push_back(i);
    }
    std::sort(alpha_.begin(), alpha_.end(), [&](int a, int b) { return words_[a].key < words_[b].key; });
}

void Lexicon::addUser(const std::string &w) {
    if (w.size() < 2 || known(w) || block_.count(w) || block_.count(strip(w))) return;
    user_.push_back(w);
    words_.push_back({w, strip(w), 0.62f});
    index();
}

bool Lexicon::loadUser(const std::string &path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string w;
    while (std::getline(in, w)) if (w.size() >= 2 && !known(w) && !block_.count(w) && !block_.count(strip(w))) { user_.push_back(w); words_.push_back({w, strip(w), 0.62f}); }
    index();
    return true;
}

void Lexicon::saveUser(const std::string &path) const {
    std::ofstream out(path, std::ios::trunc);
    for (auto &w : user_) out << w << "\n";
}

bool Lexicon::isOut(const std::string &lower) const { return byOut_.count(lower) > 0; }

bool Lexicon::known(const std::string &lower) const { return byKey_.count(lower) || byOut_.count(lower); }

float Lexicon::freq(const std::string &lower) const {
    auto it = byOut_.find(lower);
    if (it == byOut_.end()) it = byKey_.find(lower);
    return it == byKey_.end() ? 0.f : words_[it->second].f;
}

std::vector<std::string> Lexicon::complete(const std::string &prefix, int n) const {
    std::vector<int> hit;
    auto it = std::lower_bound(alpha_.begin(), alpha_.end(), prefix,
                               [&](int i, const std::string &p) { return words_[i].key < p; });
    for (; it != alpha_.end(); ++it) {
        const Word &w = words_[*it];
        if (w.key.compare(0, prefix.size(), prefix) != 0) break;
        if (w.key.size() > prefix.size()) hit.push_back(*it);
    }
    const int k = std::min<int>(n, (int)hit.size());
    std::partial_sort(hit.begin(), hit.begin() + k, hit.end(),
                      [&](int a, int b) { return words_[a].f > words_[b].f; });
    std::vector<std::string> r;
    for (int i = 0; i < k; ++i) r.push_back(words_[hit[i]].out);
    return r;
}

std::string Lexicon::correct(const std::string &w, const std::function<bool(char, char)> &neighbour) const {
    if (w.size() < 3 || known(w)) return "";
    int best = -1; float bf = -1;
    auto probe = [&](const std::string &c) {
        auto it = byKey_.find(c);
        if (it != byKey_.end() && words_[it->second].f > bf) { bf = words_[it->second].f; best = it->second; }
    };
    for (size_t i = 0; i < w.size(); ++i) {
        probe(w.substr(0, i) + w.substr(i + 1));                                   // deletion
        if (i + 1 < w.size()) { std::string t = w; std::swap(t[i], t[i + 1]); probe(t); }   // transposition
        for (char c = 'a'; c <= 'z'; ++c) {
            if (c == w[i]) continue;
            if (neighbour(w[i], c)) { std::string t = w; t[i] = c; probe(t); }      // substitution (adjacent key)
        }
    }
    for (size_t i = 0; i <= w.size(); ++i)
        for (char c = 'a'; c <= 'z'; ++c) probe(w.substr(0, i) + c + w.substr(i));   // insertion
    return best < 0 ? "" : words_[best].out;
}

std::string Lexicon::restore(const std::string &lower) const {
    auto it = byKey_.find(lower);
    if (it == byKey_.end()) return "";
    const Word &w = words_[it->second];
    return w.out != w.key ? w.out : "";
}

bool Lexicon::nextLetters(const std::string &prefix, float out[26]) const {
    for (int i = 0; i < 26; ++i) out[i] = 0;
    float tot = 0;
    auto it = std::lower_bound(alpha_.begin(), alpha_.end(), prefix,
                               [&](int i, const std::string &p) { return words_[i].key < p; });
    for (; it != alpha_.end(); ++it) {
        const Word &w = words_[*it];
        if (w.key.compare(0, prefix.size(), prefix) != 0) break;
        if (w.key.size() <= prefix.size()) continue;
        const char c = w.key[prefix.size()];
        if (c < 'a' || c > 'z') continue;
        const float wt = w.f * w.f * w.f + 0.001f;   // favour common words
        out[c - 'a'] += wt; tot += wt;
    }
    if (tot <= 0) return false;
    for (int i = 0; i < 26; ++i) out[i] /= tot;
    return true;
}
