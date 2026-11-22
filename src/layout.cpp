#include "layout.h"

#include <cmath>

namespace {

struct Spec {
    KeyType type = KeyType::Char;
    std::string label, out, alts;
    float w = 1;
    Page target = Page::Letters;
    bool accent = false, small = false;
    float padL = 0, padR = 0;
};

Spec ch(const std::string &s, const std::string &alts = "", float w = 1) {
    Spec p; p.label = p.out = s; p.alts = alts; p.w = w; return p;
}

const char *altsFor(char c) {
    switch (c) {
    case 'a': return "à á â ä æ ã å ā";
    case 'e': return "è é ê ë ē ė ę";
    case 'i': return "î ï í ī į ì";
    case 'o': return "ô ö ò ó œ ø ō õ";
    case 'u': return "û ü ù ú ū";
    case 'c': return "ç ć č";
    case 'n': return "ñ ń";
    case 's': return "ß ś š";
    case 'y': return "ý ÿ";
    case 'z': return "ž ź ż";
    case 'l': return "ł";
    case 'd': return "ð";
    case 't': return "þ";
    default: return "";
    }
}

}  // namespace

bool Layout::neighbour(char a, char b) {
    static const char *rows[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const float off[3] = {0.f, 0.5f, 1.5f};
    auto pos = [&](char c, float &x, float &y) {
        for (int r = 0; r < 3; ++r)
            for (int i = 0; rows[r][i]; ++i)
                if (rows[r][i] == c) { x = i + off[r]; y = (float)r; return true; }
        return false;
    };
    float ax, ay, bx, by;
    if (!pos(a, ax, ay) || !pos(b, bx, by)) return false;
    return std::fabs(ax - bx) <= 1.01f && std::fabs(ay - by) <= 1.01f;
}

void Layout::build(Page page, float w, float h, const Context &ctx) {
    page_ = page; w_ = w; h_ = h;
    keys_.clear();
    const float rh = rowHeight(w);
    keyW_ = w / 10; keyH_ = rh;

    auto row = [&](int r, std::vector<Spec> specs) {
        float units = 0;
        for (auto &s : specs) units += s.w;
        const float unit = w / units;
        float x = 0;
        const float y = kStrip + r * rh;
        for (auto &s : specs) {
            Key k;
            k.type = s.type; k.label = s.label; k.out = s.out; k.alts = s.alts; k.target = s.target;
            k.accent = s.accent; k.small = s.small;
            k.x = x; k.y = y; k.w = s.w * unit; k.h = rh;
            k.padL = s.padL * unit; k.padR = s.padR * unit;
            if (s.type == KeyType::Char && s.label.size() == 1 && s.label[0] >= 'a' && s.label[0] <= 'z' && page == Page::Letters)
                k.letter = s.label[0] - 'a';
            x += k.w;
            keys_.push_back(k);
        }
    };
    auto digits = [&] {
        std::vector<Spec> r;
        for (char c : std::string("1234567890")) r.push_back(ch(std::string(1, c)));
        row(0, r);
    };
    auto page2 = [&](const std::string &label, Page t, float width, bool small = true) {
        Spec s; s.type = KeyType::Page; s.label = label; s.target = t; s.w = width; s.small = small; return s;
    };
    auto bs = [&](float width) { Spec s; s.type = KeyType::Backspace; s.label = "bksp"; s.w = width; return s; };
    auto enter = [&](float width) { Spec s; s.type = KeyType::Enter; s.label = "enter"; s.w = width; s.accent = true; return s; };
    auto space = [&](float width) { Spec s; s.type = KeyType::Space; s.label = " "; s.out = " "; s.w = width; return s; };

    switch (page) {
    case Page::Letters: {
        digits();
        std::vector<Spec> r1, r2, r3, r4;
        for (char c : std::string("qwertyuiop")) r1.push_back(ch(std::string(1, c), altsFor(c)));
        row(1, r1);
        for (char c : std::string("asdfghjkl")) r2.push_back(ch(std::string(1, c), altsFor(c)));
        r2.front().w += 0.5f; r2.front().padL = 0.5f;
        r2.back().w += 0.5f; r2.back().padR = 0.5f;
        row(2, r2);
        { Spec s; s.type = KeyType::Shift; s.label = "shift"; s.w = 1.5f; r3.push_back(s); }
        for (char c : std::string("zxcvbnm")) r3.push_back(ch(std::string(1, c), altsFor(c)));
        r3.push_back(bs(1.5f));
        row(3, r3);
        r4.push_back(page2("?123", Page::Sym1, 1.5f));
        if (ctx.email) r4.push_back(ch("@", "_ - . +"));
        else if (ctx.url) r4.push_back(ch("/", ". : ? & = #"));
        else if (ctx.terminal) r4.push_back(ch("-", "_ | ~ \\ ` $ & * = + ; : ' \""));
        else r4.push_back(ch(",", "' \" ; : ! ? ( ) -"));
        r4.push_back(space(5));
        r4.push_back(ch(".", ", ? ! : ; - _ ' \" ( ) @ / &"));
        r4.push_back(enter(1.5f));
        row(4, r4);
        break;
    }
    case Page::Sym1: {
        digits();
        std::vector<Spec> r1, r2, r3, r4;
        for (char c : std::string("@#$_&-+()/")) r1.push_back(ch(std::string(1, c)));
        row(1, r1);
        r2.push_back(page2("=\\<", Page::Sym2, 1.5f));
        for (char c : std::string("*\"':;!?")) r2.push_back(ch(std::string(1, c)));
        r2.push_back(bs(1.5f));
        row(2, r2);
        // keep five rows: row 3 = more punctuation, row 4 = page/space/enter
        for (char c : std::string("~`|^%=[]{}")) r3.push_back(ch(std::string(1, c)));
        row(3, r3);
        r4.push_back(page2("ABC", Page::Letters, 1.5f));
        r4.push_back(ch(","));
        r4.push_back(space(5));
        r4.push_back(ch("."));
        r4.push_back(enter(1.5f));
        row(4, r4);
        break;
    }
    case Page::Sym2: {
        digits();
        std::vector<Spec> r1, r2, r3, r4;
        for (const char *c : {"€", "£", "¥", "¢", "°", "©", "®", "™", "§", "¶"}) r1.push_back(ch(c));
        row(1, r1);
        r2.push_back(page2("?123", Page::Sym1, 1.5f));
        for (const char *c : {"<", ">", "«", "»", "“", "”", "‘"}) r2.push_back(ch(c));
        r2.push_back(bs(1.5f));
        row(2, r2);
        for (const char *c : {"\\", "•", "√", "π", "÷", "×", "±", "∞", "≈", "≠"}) r3.push_back(ch(c));
        row(3, r3);
        r4.push_back(page2("ABC", Page::Letters, 1.5f));
        r4.push_back(ch(","));
        r4.push_back(space(5));
        r4.push_back(ch("."));
        r4.push_back(enter(1.5f));
        row(4, r4);
        break;
    }
    case Page::Numpad: {
        // 4 columns x 4 rows stretched over the five-row height
        const float nr = rh * 5.f / 4.f;
        auto nrow = [&](int r, std::vector<Spec> specs) {
            float x = 0; const float cw = w / 4;
            for (auto &s : specs) {
                Key k; k.type = s.type; k.label = s.label; k.out = s.out; k.target = s.target; k.accent = s.accent; k.small = s.small;
                k.x = x; k.y = kStrip + r * nr; k.w = cw * s.w; k.h = nr; x += k.w; keys_.push_back(k);
            }
        };
        nrow(0, {ch("1"), ch("2"), ch("3"), bs(1)});
        nrow(1, {ch("4"), ch("5"), ch("6"), ch(",")});
        nrow(2, {ch("7"), ch("8"), ch("9"), ch(".")});
        nrow(3, {page2("ABC", Page::Letters, 1), ch("0"), ch("-"), enter(1)});
        break;
    }
    }
    if (page == Page::Letters) {
        for (const Key &k : keys_)
            if (k.letter >= 0) centres_[k.letter] = {k.x + k.padL + (k.w - k.padL - k.padR) / 2, k.y + k.h / 2};
    }
}

int Layout::keyAt(float x, float y) const {
    if (y < kStrip) return -1;
    int best = -1; float bd = 1e30f;
    for (int i = 0; i < (int)keys_.size(); ++i) {
        const Key &k = keys_[i];
        if (x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h) return i;
        const float dx = std::fmax(0.f, std::fmax(k.x - x, x - (k.x + k.w)));
        const float dy = std::fmax(0.f, std::fmax(k.y - y, y - (k.y + k.h)));
        const float d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
