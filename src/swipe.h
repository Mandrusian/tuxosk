// Swipe (glide) typing decoder: SHARK2-style shape match of the finger path against the ideal
// polyline through the key centres of every candidate word, plus key-coverage and word frequency.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "lexicon.h"

struct Pt { float x = 0, y = 0; };

class SwipeDecoder {
public:
    static constexpr int N = 32;   // resampled points per path

    // centres[0..25] = key centres of a..z in layout pixels, keyW/keyH = key size
    void setLayout(const std::array<Pt, 26> &centres, float keyW, float keyH);
    void setLexicon(const Lexicon *lex);
    // best `n` words (display form) with a cost (lower is better), most likely first
    std::vector<std::pair<std::string, float>> decode(const std::vector<Pt> &path, int n) const;

private:
    struct Tpl {
        int word;               // index into lexicon words
        uint8_t first, last;    // letter indices
        float len;              // polyline length
        std::array<Pt, N> pts;
    };
    void build();
    std::array<Pt, 26> c_{};
    float kw_ = 50, kh_ = 50;
    const Lexicon *lex_ = nullptr;
    std::vector<Tpl> tpl_;
};
