// Key geometry for the keyboard pages. All coordinates are logical pixels, origin top-left of the surface.
#pragma once
#include <array>
#include <string>
#include <vector>

#include "swipe.h"

enum class KeyType { Char, Shift, Backspace, Enter, Space, Page };
enum class Page { Letters, Sym1, Sym2, Numpad };

struct Key {
    KeyType type = KeyType::Char;
    std::string label;                 // drawn text (letters follow shift state in the UI)
    std::string out;                   // typed text (Char / Space)
    float x = 0, y = 0, w = 0, h = 0;  // full cell, no gap
    int letter = -1;                   // 0..25 for a-z keys on the letters page, else -1
    Page target = Page::Letters;       // Page keys
    std::string alts;                  // long-press alternates, space separated
    bool accent = false;               // enter key
    bool small = false;                // small label (page toggles)
    float padL = 0, padR = 0;          // blank margin inside the cell (half-key indent of the home row)
};

struct Context {
    bool email = false, url = false, numeric = false, terminal = false;
};

class Layout {
public:
    static constexpr float kStrip = 46;     // suggestion strip height
    // total logical height for a given width (rows stay finger sized on any phone width)
    static float heightFor(float width) { return kStrip + 5 * rowHeight(width) + 6; }
    static inline float heightScale = 1.0f;   // 1 normal, ~1.12 for the "taller keys" setting
    static float rowHeight(float width) { return width * 0.120f * heightScale; }

    void build(Page page, float w, float h, const Context &ctx);
    const std::vector<Key> &keys() const { return keys_; }
    // key under a point (cells are contiguous, so every point inside a row maps to a key); -1 = strip/none
    int keyAt(float x, float y) const;
    Page page() const { return page_; }
    float width() const { return w_; }
    float height() const { return h_; }
    // a..z centres of the letters page (for swipe decoding), valid after build(Page::Letters, ...)
    const std::array<Pt, 26> &centres() const { return centres_; }
    float keyW() const { return keyW_; }
    float keyH() const { return keyH_; }
    // adjacent letter keys (autocorrect substitutions)
    static bool neighbour(char a, char b);

private:
    Page page_ = Page::Letters;
    float w_ = 0, h_ = 0, keyW_ = 0, keyH_ = 0;
    std::vector<Key> keys_;
    std::array<Pt, 26> centres_{};
};
