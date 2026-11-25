#include "engine.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "layout.h"

using Clock = std::chrono::steady_clock;

namespace {

bool isAlnum(char c) { return std::isalnum((unsigned char)c) || (unsigned char)c >= 0x80; }

std::string stripApos(const std::string &s) {
    std::string r;
    for (char c : s) if (c != '\'') r += c;
    return r;
}

}  // namespace

std::string Engine::lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string Engine::applyCase(const std::string &typed, const std::string &fix) {
    if (typed.empty() || fix.empty()) return fix;
    std::string r = fix;
    bool allUpper = typed.size() > 1;
    for (char c : typed) if (std::isalpha((unsigned char)c) && !std::isupper((unsigned char)c)) { allUpper = false; break; }
    if (allUpper) { for (char &c : r) c = (char)std::toupper((unsigned char)c); }
    else if (std::isupper((unsigned char)typed[0])) r[0] = (char)std::toupper((unsigned char)r[0]);
    return r;
}

void Engine::push(const std::string &s) {
    before_ += s;
    if (before_.size() > 128) {
        size_t cut = before_.size() - 96;
        while (cut < before_.size() && ((unsigned char)before_[cut] & 0xC0) == 0x80) ++cut;
        before_.erase(0, cut);
    }
    lastCommit_ = Clock::now();
}

bool Engine::needsCap() const {
    size_t e = before_.size(), k = 0;
    while (e > 0 && before_[e - 1] == ' ') { --e; ++k; }
    if (e == 0) return k == 0 ? true : before_.empty();
    const char c = before_[e - 1];
    if (c == '\n') return true;
    return k >= 1 && (c == '.' || c == '!' || c == '?');
}

void Engine::autoCap() {
    if (noPredict_ || !autocapOn_) return;
    if (shift_ == Off && needsCap()) { shift_ = Once; changed(); }
}

bool Engine::charToKey(const std::string &s, uint32_t &code, uint32_t &mods) {
    if (s.size() != 1) return false;
    static const char *rowQ = "qwertyuiop", *rowA = "asdfghjkl", *rowZ = "zxcvbnm";
    const unsigned char c = (unsigned char)s[0];
    mods = 0;
    if (c >= 'A' && c <= 'Z') { const bool ok = charToKey(std::string(1, (char)(c + 32)), code, mods); mods = 1; return ok; }
    for (int i = 0; rowQ[i]; ++i) if (rowQ[i] == c) { code = 16 + i; return true; }
    for (int i = 0; rowA[i]; ++i) if (rowA[i] == c) { code = 30 + i; return true; }
    for (int i = 0; rowZ[i]; ++i) if (rowZ[i] == c) { code = 44 + i; return true; }
    if (c >= '1' && c <= '9') { code = 2 + (c - '1'); return true; }
    if (c == '0') { code = 11; return true; }
    struct M { char c; uint32_t code, mods; };
    static const M m[] = {
        {' ', 57, 0}, {'-', 12, 0}, {'=', 13, 0}, {'[', 26, 0}, {']', 27, 0}, {';', 39, 0}, {'\'', 40, 0}, {'`', 41, 0},
        {'\\', 43, 0}, {',', 51, 0}, {'.', 52, 0}, {'/', 53, 0}, {'\n', 28, 0}, {'\t', 15, 0},
        {'!', 2, 1}, {'@', 3, 1}, {'#', 4, 1}, {'$', 5, 1}, {'%', 6, 1}, {'^', 7, 1}, {'&', 8, 1}, {'*', 9, 1},
        {'(', 10, 1}, {')', 11, 1}, {'_', 12, 1}, {'+', 13, 1}, {'{', 26, 1}, {'}', 27, 1}, {':', 39, 1}, {'"', 40, 1},
        {'~', 41, 1}, {'|', 43, 1}, {'<', 51, 1}, {'>', 52, 1}, {'?', 53, 1}};
    for (auto &e : m) if (e.c == (char)c) { code = e.code; mods = e.mods; return true; }
    return false;
}

void Engine::typeDirect(const std::string &s) {
    uint32_t code, mods;
    std::string t = s;
    if (shift_ != Off && t.size() == 1 && std::isalpha((unsigned char)t[0])) t[0] = (char)std::toupper((unsigned char)t[0]);
    if (charToKey(t, code, mods)) out_.key(code, mods);
    else out_.commit(t);               // not on a US layout: best effort through the input method
    out_.flush();
    push(t);
    if (shift_ == Once) { shift_ = Off; changed(); }
}

void Engine::deleteChars(int n) {
    if (n <= 0) return;
    if (out_.imActive()) out_.del(n, 0);
    else for (int i = 0; i < n; ++i) out_.key(K_BACKSPACE, 0);
    before_.erase(before_.size() >= (size_t)n ? before_.size() - n : 0);
}

std::string Engine::finishWord(bool autocorrect, std::string *typedOut) {
    if (word_.empty()) return "";
    const std::string typed = word_;
    word_.clear();
    if (typedOut) *typedOut = typed;
    if (!autocorrect || noPredict_ || !autocorrectOn_) { lastAuto_.active = false; return typed; }
    const std::string lw = lower(typed);
    std::string fix;
    if (!lex_.isOut(lw)) {
        fix = lex_.restore(stripApos(lw));
        if (fix.empty() && !lex_.known(stripApos(lw))) fix = lex_.correct(stripApos(lw), Layout::neighbour);
    }
    if (!fix.empty()) {
        const std::string text = applyCase(typed, fix);
        lastAuto_ = {true, typed, text};
        return text;
    }
    lastAuto_.active = false;
    if (lw.size() >= 3 && !lex_.known(stripApos(lw)) && ++seen_[lw] >= 2) lex_.addUser(lw);
    return typed;
}

void Engine::typeChar(const std::string &s) {
    if (s.empty()) return;
    lastAuto_.active = false;
    if (!out_.imActive()) { typeDirect(s); autoCap(); return; }
    if (noPredict_) {
        std::string t = s;
        if (shift_ != Off && t.size() == 1 && std::isalpha((unsigned char)t[0])) t[0] = (char)std::toupper((unsigned char)t[0]);
        out_.commit(t); out_.flush(); push(t);
        if (shift_ == Once) { shift_ = Off; changed(); }
        return;
    }
    const bool letter = s.size() == 1 && std::isalpha((unsigned char)s[0]);
    const bool apos = s == "'" && !word_.empty();
    const bool wasAuto = autoSpace_;
    afterSwipe_ = false; lastSwipeBytes_ = 0; autoSpace_ = false;
    if (letter || apos) {
        std::string t = s;
        if (letter && shift_ != Off) t[0] = (char)std::toupper((unsigned char)t[0]);
        word_ += t;
        out_.preedit(word_);
        out_.flush();
        lastCommit_ = Clock::now();
        if (shift_ == Once) { shift_ = Off; changed(); }
        refreshSuggestions();
        return;
    }
    const std::string text = finishWord(true);
    if (lastAuto_.active) lastAuto_.suffixBytes = (int)s.size();
    if (wasAuto && text.empty() && s.size() == 1 && std::strchr(".,!?;:)", s[0]) && !before_.empty() && before_.back() == ' ')
        deleteChars(1);                 // the space we added after a swipe/suggestion goes away before punctuation
    out_.commit(text + s);
    out_.flush();
    push(text + s);
    refreshSuggestions();
    autoCap();
}

void Engine::space() {
    const auto now = Clock::now();
    if (!out_.imActive()) {
        typeDirect(" ");
        lastSpace_ = now;
        autoCap();
        return;
    }
    afterSwipe_ = false; lastSwipeBytes_ = 0;
    if (!word_.empty() && !noPredict_) {
        const std::string text = finishWord(true);
        out_.commit(text + " ");
        out_.flush();
        push(text + " ");
        autoSpace_ = false;
        lastSpace_ = now;
    } else {
        const bool dbl = !noPredict_ && now - lastSpace_ < std::chrono::milliseconds(500) && before_.size() >= 2 &&
                         before_.back() == ' ' && isAlnum(before_[before_.size() - 2]);
        if (dbl) { deleteChars(1); out_.commit(". "); push(". "); }
        else { out_.commit(" "); push(" "); }
        out_.flush();
        lastSpace_ = now;
        lastAuto_.active = false;
    }
    autoCap();
    refreshSuggestions();
}

void Engine::backspace() {
    if (!word_.empty()) {
        do word_.pop_back(); while (!word_.empty() && ((unsigned char)word_.back() & 0xC0) == 0x80);
        out_.preedit(word_);
        out_.flush();
        refreshSuggestions();
        return;
    }
    if (lastAuto_.active) {
        // undo the autocorrect: the typed word comes back as composing text
        const std::string typed = lastAuto_.typed, fixed = lastAuto_.fixed;
        const int suffix = lastAuto_.suffixBytes;
        lastAuto_.active = false;
        deleteChars((int)fixed.size() + suffix);
        word_ = typed;
        out_.preedit(word_);
        out_.flush();
        lex_.addUser(lower(typed));
        refreshSuggestions();
        return;
    }
    if (afterSwipe_ && lastSwipeBytes_ > 0) {
        deleteChars(lastSwipeBytes_);
        out_.flush();
        afterSwipe_ = false; lastSwipeBytes_ = 0;
        sugg_.clear(); main_ = 0; literal_ = -1;
        changed();
        autoCap();
        return;
    }
    out_.key(K_BACKSPACE, 0);
    out_.flush();
    if (!before_.empty()) {
        do before_.pop_back(); while (!before_.empty() && ((unsigned char)before_.back() & 0xC0) == 0x80);
    }
    lastCommit_ = Clock::now();
    autoCap();
    refreshSuggestions();
}

void Engine::enter() {
    const std::string text = finishWord(true);
    if (!text.empty()) { out_.commit(text); push(text); }
    out_.flush();
    out_.key(K_ENTER, 0);
    out_.flush();
    push("\n");
    afterSwipe_ = false; lastSwipeBytes_ = 0; lastAuto_.active = false;
    autoCap();
    refreshSuggestions();
}

void Engine::tab() {
    const std::string text = finishWord(false);
    if (!text.empty()) { out_.commit(text); push(text); }
    out_.key(K_TAB, 0);
    out_.flush();
}

void Engine::moveCursor(int dir) {
    const std::string text = finishWord(false);
    if (!text.empty()) { out_.commit(text); push(text); }
    out_.key(dir < 0 ? K_LEFT : K_RIGHT, 0);
    out_.flush();
    afterSwipe_ = false; lastSwipeBytes_ = 0; lastAuto_.active = false;
}

void Engine::tapShift() {
    shift_ = shift_ == Off ? Once : Off;
    changed();
}

void Engine::doubleTapShift() {
    shift_ = Lock;
    changed();
}

void Engine::swipe(const std::vector<Pt> &path) {
    if (noPredict_ || !out_.imActive()) return;
    auto cands = dec_.decode(path, 3);
    if (cands.empty()) return;
    std::string pre;
    if (!word_.empty()) {
        const std::string t = finishWord(true);
        pre = t + " ";
    } else if (!before_.empty() && isAlnum(before_.back())) {
        pre = " ";
    } else if (!before_.empty() && (before_.back() == '.' || before_.back() == ',' || before_.back() == '!' ||
                                    before_.back() == '?' || before_.back() == ':' || before_.back() == ';')) {
        pre = " ";
    }
    std::string w = cands[0].first;
    if (shift_ == Lock) for (char &c : w) c = (char)std::toupper((unsigned char)c);
    else if (shift_ == Once) w[0] = (char)std::toupper((unsigned char)w[0]);
    out_.commit(pre + w + " ");
    out_.flush();
    push(pre + w + " ");
    lastSwipeWord_ = w;
    lastSwipeBytes_ = (int)w.size() + 1;
    afterSwipe_ = true; autoSpace_ = true; lastAuto_.active = false;
    sugg_.clear();
    for (auto &c : cands) {
        std::string t = c.first;
        sugg_.push_back(applyCase(w, t));
    }
    main_ = 0; literal_ = -1;
    if (shift_ == Once) shift_ = Off;
    changed();
}

void Engine::pickSuggestion(int i) {
    if (i < 0 || i >= (int)sugg_.size()) return;
    const std::string pick = sugg_[i];
    if (afterSwipe_ && lastSwipeBytes_ > 0) {
        const std::string text = applyCase(lastSwipeWord_, pick);
        deleteChars(lastSwipeBytes_);
        out_.commit(text + " ");
        out_.flush();
        push(text + " ");
        lastSwipeWord_ = text;
        lastSwipeBytes_ = (int)text.size() + 1;
        main_ = i;
        changed();
        return;
    }
    if (!word_.empty()) {
        const bool literal = i == literal_;
        const std::string typed = word_;
        word_.clear();
        const std::string text = literal ? typed : applyCase(typed, pick);
        if (literal) lex_.addUser(lower(typed));
        out_.commit(text + " ");
        out_.flush();
        push(text + " ");
        autoSpace_ = true; lastAuto_.active = false;
    } else {
        out_.commit(pick + " ");
        out_.flush();
        push(pick + " ");
        autoSpace_ = true;
    }
    afterSwipe_ = false; lastSwipeBytes_ = 0;
    autoCap();
    refreshSuggestions();
}

void Engine::activated() {
    word_.clear(); lastAuto_.active = false; afterSwipe_ = false; lastSwipeBytes_ = 0; autoSpace_ = false;
    lastCommit_ = {};
    autoCap();
    refreshSuggestions();
}

void Engine::deactivated() {
    word_.clear(); afterSwipe_ = false; lastSwipeBytes_ = 0;
    refreshSuggestions();
}

void Engine::surrounding(const std::string &text, uint32_t cursor) {
    if (Clock::now() - lastCommit_ < std::chrono::milliseconds(400)) return;   // our own edits are still in flight
    size_t c = std::min<size_t>(cursor, text.size());
    std::string b = text.substr(0, c);
    if (b.size() > 96) {
        size_t cut = b.size() - 96;
        while (cut < b.size() && ((unsigned char)b[cut] & 0xC0) == 0x80) ++cut;
        b.erase(0, cut);
    }
    before_ = b;
    autoCap();
}

void Engine::setNoPredict(bool v) {
    noPredict_ = v;
    if (v) { word_.clear(); sugg_.clear(); if (shift_ == Once) shift_ = Off; changed(); }
}

void Engine::refreshSuggestions() {
    sugg_.clear(); literal_ = -1; main_ = 0;
    if (noPredict_ || !out_.imActive()) { changed(); return; }
    if (word_.empty()) {
        // nothing typed: a few common words (after a swipe the strip keeps its alternatives instead)
        if (!afterSwipe_) {
            if (needsCap() || shift_ != Off) sugg_ = {"I", "The", "I'm"};
            else sugg_ = {"I", "the", "and"};
        }
        changed();
        return;
    }
    const std::string lw = lower(word_), key = stripApos(lw);
    std::vector<std::string> comp = lex_.complete(key, 4);
    if (!lex_.known(key)) {
        literal_ = 0;
        sugg_.push_back(word_);
        const std::string fix = lex_.correct(key, Layout::neighbour);
        if (!fix.empty()) { sugg_.push_back(applyCase(word_, fix)); main_ = 1; }
    } else {
        sugg_.push_back(word_);
        if (!lex_.isOut(lw)) {
            const std::string fix = lex_.restore(key);
            if (!fix.empty()) { literal_ = 0; sugg_.push_back(applyCase(word_, fix)); main_ = 1; }
        }
    }
    for (auto &c : comp) {
        if (sugg_.size() >= 3) break;
        const std::string t = applyCase(word_, c);
        if (std::find(sugg_.begin(), sugg_.end(), t) == sugg_.end()) sugg_.push_back(t);
    }
    changed();
}

void Engine::combo(const std::string &utf8, uint32_t mods) {
    uint32_t c, m;
    if (!charToKey(utf8, c, m)) return;
    out_.key(c, m | mods);
    out_.flush();
}

void Engine::pressKey(uint32_t code, uint32_t mods) {
    out_.key(code, mods);
    out_.flush();
}
