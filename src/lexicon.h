// Word list with frequencies: completion, autocorrect, swipe candidates.
#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct Word {
    std::string out;   // what gets typed ("don't")
    std::string key;   // letters only, lowercase ("dont"): what a swipe/typed run spells
    float f = 0;       // 0..1 normalised log frequency
};

class Lexicon {
public:
    // "word count" per line (FrequencyWords format). Returns false if the file cannot be read.
    bool load(const std::string &path);
    // words never suggested or swiped (offensive terms), one per line; call before load()
    void loadBlocklist(const std::string &path);
    // learned word (typed twice or kept literal): boosted so it completes and survives autocorrect
    void addUser(const std::string &w);
    bool loadUser(const std::string &path);
    void saveUser(const std::string &path) const;

    const std::vector<Word> &words() const { return words_; }
    bool known(const std::string &lower) const;                       // exact key or out match
    bool isOut(const std::string &lower) const;                       // exact typed form ("its", "don't")
    float freq(const std::string &lower) const;                       // 0 when unknown
    // best `n` completions of a lowercase prefix, most frequent first (prefix itself excluded)
    std::vector<std::string> complete(const std::string &prefix, int n) const;
    // probability-ish weight (sums to 1) of each next letter after a lowercase prefix; false when no word continues it
    bool nextLetters(const std::string &prefix, float out[26]) const;
    // best single edit-distance-1 fix of an unknown lowercase word, or ""
    // neighbour(a,b) tells whether two letters are adjacent keys (substitution only allowed then)
    std::string correct(const std::string &lower, const std::function<bool(char, char)> &neighbour) const;
    // contraction/apostrophe form for a stripped word ("dont" -> "don't"), or ""
    std::string restore(const std::string &lower) const;

private:
    void index();
    std::vector<Word> words_;
    std::unordered_map<std::string, int> byKey_;   // key -> index (highest freq wins)
    std::unordered_map<std::string, int> byOut_;
    std::vector<int> alpha_;                       // indices sorted by key
    std::vector<std::string> user_;
    std::unordered_set<std::string> block_;
};
