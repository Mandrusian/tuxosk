// Text logic: composing word, suggestions, autocorrect, swipe commits, shift state.
// Talks to the application through the abstract Output (input-method + virtual keyboard in main.cpp).
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "lexicon.h"
#include "swipe.h"

struct Output {
    virtual ~Output() = default;
    virtual void commit(const std::string &s) = 0;              // insert text (replaces any preedit)
    virtual void preedit(const std::string &s) = 0;             // underlined composing text, "" clears
    virtual void del(int before, int after) = 0;                // delete surrounding text, bytes
    virtual void key(uint32_t evdev, uint32_t mods) = 0;        // key press+release; mods: 1 shift, 2 ctrl
    virtual void flush() = 0;                                   // apply the batch above
    virtual bool imActive() const = 0;                          // a text field has the input method
};

enum KeyCode : uint32_t {
    K_ESC = 1, K_BACKSPACE = 14, K_TAB = 15, K_ENTER = 28, K_SPACE = 57, K_LEFT = 105, K_RIGHT = 106, K_UP = 103, K_DOWN = 108
};

class Engine {
public:
    enum Shift { Off, Once, Lock };

    Engine(Lexicon &lex, SwipeDecoder &dec, Output &out) : lex_(lex), dec_(dec), out_(out) {}

    // user actions
    void typeChar(const std::string &utf8);      // a character key (letters are cased by the shift state)
    void space();
    void backspace();
    void enter();
    void tab();
    void moveCursor(int dir);                    // -1 left, +1 right
    void tapShift();
    void doubleTapShift();
    void swipe(const std::vector<Pt> &path);
    void pickSuggestion(int i);
    void combo(const std::string &utf8, uint32_t mods);   // Ctrl/Alt chords: mods 1 shift, 2 ctrl, 4 alt
    void pressKey(uint32_t code, uint32_t mods = 0);

    // field state (from the input method)
    void activated();                            // new text field: drop composing state
    void deactivated();
    void surrounding(const std::string &text, uint32_t cursor);
    void setAutocorrect(bool v) { autocorrectOn_ = v; }
    void setAutocap(bool v) { autocapOn_ = v; }
    void setNoPredict(bool v);                   // passwords, pin fields: plain commits, no suggestions

    // for the UI
    Shift shift() const { return shift_; }
    const std::vector<std::string> &suggestions() const { return sugg_; }
    int mainSuggestion() const { return main_; }
    const std::string &composing() const { return word_; }
    std::function<void()> onChange;              // suggestions or shift changed

    static bool charToKey(const std::string &utf8, uint32_t &code, uint32_t &mods);

private:
    std::string finishWord(bool autocorrect, std::string *typedOut = nullptr);
    void typeDirect(const std::string &utf8);
    void deleteChars(int n);
    void refreshSuggestions();
    void autoCap();
    bool needsCap() const;
    void changed() { if (onChange) onChange(); }
    void push(const std::string &s);
    static std::string lower(std::string s);
    static std::string applyCase(const std::string &typed, const std::string &fix);

    Lexicon &lex_;
    SwipeDecoder &dec_;
    Output &out_;
    Shift shift_ = Off;
    std::string word_;                 // composing word as typed
    std::string before_;               // text before the cursor (tail)
    std::vector<std::string> sugg_;
    int main_ = 0;
    bool noPredict_ = false, autocorrectOn_ = true, autocapOn_ = true;
    bool afterSwipe_ = false;          // alternatives in the strip replace the last swiped word
    int lastSwipeBytes_ = 0;
    struct { bool active = false; std::string typed, fixed; } lastAuto_;
    std::chrono::steady_clock::time_point lastCommit_{}, lastSpace_{};
    bool autoSpace_ = false;           // trailing space was inserted by us (swipe / suggestion)
    int literal_ = -1;                 // index of the literal typed word in the strip, -1 none
    std::string lastSwipeWord_;
    std::unordered_map<std::string, int> seen_;   // unknown words kept literal (learned on the 2nd time)
};
