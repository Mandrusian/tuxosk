// Touch handling and software rendering of the keyboard (cairo). No Wayland in here: main.cpp feeds it
// touch events and blits the damaged pixels into a wl_shm buffer.
#pragma once
#include <cairo.h>

#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "engine.h"
#include "layout.h"
#include "settings.h"

struct PxRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;      // [x0,x1) x [y0,y1), device pixels
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    void add(const PxRect &o) {
        if (o.empty()) return;
        if (empty()) { *this = o; return; }
        x0 = std::min(x0, o.x0); y0 = std::min(y0, o.y0); x1 = std::max(x1, o.x1); y1 = std::max(y1, o.y1);
    }
};

struct Timers {
    virtual ~Timers() = default;
    virtual int add(int ms, std::function<bool()> cb) = 0;      // cb returns true to repeat
    virtual void remove(int id) = 0;
};

struct Theme {
    unsigned bg = 0x202124, key = 0x3c4043, special = 0x2d2f31, pressed = 0x5f6368, text = 0xe8eaed,
             dim = 0x9aa0a6, accent = 0x8ab4f8, onAccent = 0x202124, popup = 0x5f6368, dialog = 0x2b2d30, sel = 0x3d4b66;
};

class Ui {
public:
    Ui(Engine &e, Lexicon &lex, SwipeDecoder &dec, Timers &t);
    ~Ui();

    void resize(int logicalW, int scale);         // height follows the width
    float logicalW() const { return w_; }
    float logicalH() const { return h_; }
    int scale() const { return scale_; }
    int pxW() const { return (int)(w_ * scale_); }
    int pxH() const { return (int)(h_ * scale_); }

    void setContext(const Context &c);            // email / url / numeric fields
    void setTheme(const Theme &t) { th_ = t; redrawAll(); }
    std::function<void()> onHide;                 // hide button in the strip
    // settings dialog: ui edits *settings, then reports which key changed ("language", "filter_slurs", ...)
    struct Lang { std::string code, label, sub; };
    void setSettings(Settings *s) { set_ = s; }
    void setLanguages(std::vector<Lang> l) { langs_ = std::move(l); }
    struct Filter { std::string id, label; };
    void setFilters(std::vector<Filter> f) { filters_ = std::move(f); }
    std::function<void(const std::string &)> onBlockWord;   // word typed in "Block a word" mode
    std::function<void(const std::string &)> onSettingChanged;
    void openLangs() { openModal(Modal::Langs); }
    void openSettings() { openModal(Modal::Prefs); }
    void openFilters() { openModal(Modal::Filters); }
    bool modalOpen() const { return modal_ != Modal::None; }
    // the dialog lives on its own full-screen surface: main creates it on onModalVisible(true) and feeds its size/touches
    std::function<void(bool)> onModalVisible;
    void setModalSize(float w, float h) { mw_ = w; mh_ = h; modalDirty_ = true; }
    float modalW() const { return mw_; }
    float modalH() const { return mh_; }
    bool takeModalDirty() { const bool d = modalDirty_; modalDirty_ = false; return d; }
    void blitModal(unsigned char *dst, int stride);       // dim + dialog, ARGB32, size = modal size * scale
    void modalTouchDown(float x, float y) { modalX_ = x; modalY_ = y; modalDown_ = true; }
    void modalTouchUp() { if (modalDown_) { modalDown_ = false; modalTap(modalX_, modalY_); } }
    void closeModal();                            // animated unless motion is reduced
    void closeModalNow();
    static Theme lightTheme();
    void reset();                                 // drop touches (hidden/shown)

    // input, logical coordinates
    void touchDown(int id, float x, float y);
    void touchMotion(int id, float x, float y);
    void touchUp(int id);
    void touchCancel(int id);

    // rendering
    PxRect takeDirty();                           // damaged region since the last call (device pixels)
    bool hasDirty() const { return !dirty_.empty(); }
    void blit(unsigned char *dst, int stride, const PxRect &r);   // base + overlays into dst (ARGB32)
    void invalidateAll() { dirty_.add({0, 0, pxW(), pxH()}); }
    bool savePng(const char *path);
    // test hooks: centre of a key by label ("a", "shift", "bksp", "enter", " ", "?123", "Esc"...), path through letters of a word
    bool keyPos(const std::string &label, float &x, float &y) const;
    std::vector<Pt> wordPath(const std::string &word) const;               // debug: current frame

private:
    struct Touch {
        int id = 0;
        float x0 = 0, y0 = 0, x = 0, y = 0;
        int key = -1;                  // key under the finger (-1 strip)
        int slot = -1;                 // suggestion slot when in the strip
        bool swipe = false, longOpen = false, spaceDrag = false, repeating = false, consumed = false;
        std::vector<Pt> path;
        std::vector<std::chrono::steady_clock::time_point> stamp;
        int lpTimer = 0, repTimer = 0;
        float cursorX = 0;
        int altSel = -1;
        bool opened = false;           // long-press on space opened the dialog: release types nothing
    };

    void rebuild();
    void redrawAll();
    void redrawKey(int i);
    void redrawStrip();
    void drawKey(cairo_t *cr, int i, bool pressed);
    void drawStripInto(cairo_t *cr);
    void drawOverlays(cairo_t *cr, const PxRect &clip);
    void invalidate(float x, float y, float w, float h);
    void invalidateTrail();
    std::string labelFor(const Key &k) const;
    std::string typed(const Key &k) const;           // text a char key produces (shift applied)
    bool pressedKey(int i) const;
    void startLongPress(Touch &t);
    void openAlts(Touch &t);
    void altGeometry(const Touch &t, float &x, float &y, float &cw, float &ch, std::vector<std::string> &alts) const;
    void release(Touch &t);
    void swipeEnd(Touch &t);
    void onEngineChange();
    enum class Modal { None, Langs, Prefs, Filters };
    struct Row { float x, y, w, h; int id; };
    void layoutModal(std::vector<Row> &rows, float &dx, float &dy, float &dw, float &dh) const;
    void drawModal(cairo_t *cr);
    void modalTap(float x, float y);
    bool getFlag(int i) const;
    void setFlag(int i);
    void openModal(Modal m);
    static constexpr float kHide = 48;
    int pickKey(float x, float y) const;            // key for a tap: nearest, biased to likely next letters

    Engine &eng_;
    Lexicon &lex_;
    SwipeDecoder &dec_;
    Timers &timers_;
    Theme th_;
    Context ctx_;
    Layout lay_, letters_;
    Page page_ = Page::Letters;
    Modal modal_ = Modal::None;
    bool modalDown_ = false, modalDirty_ = false, closing_ = false;
    float modalT_ = 1.f;               // dialog open animation 0..1
    std::map<int, float> fade_;        // released keys fading out of the pressed colour
    std::set<int> lit_;                // keys currently drawn pressed
    int animId_ = 0;
    bool reduced() const { return set_ && set_->reduceMotion; }
    void startAnim();
    bool animTick();
    void finishClose();
    float keyAmount(int i) const;
    float modalX_ = 0, modalY_ = 0, mw_ = 0, mh_ = 0;
    Settings *set_ = nullptr;
    std::vector<Lang> langs_;
    std::vector<Filter> filters_;
    bool capture_ = false;             // typing a word to block
    std::string captured_;
    void bsKey();
    void finishCapture(bool save);
    bool term_ = false;
    unsigned mods_ = 0;                // sticky Ctrl (2) / Alt (4) in terminal mode
    float w_ = 0, h_ = 0;
    int scale_ = 1;
    cairo_surface_t *base_ = nullptr;
    PxRect dirty_, trailBox_;
    std::map<int, Touch> touches_;
    Engine::Shift lastShift_ = Engine::Off;
    std::chrono::steady_clock::time_point lastShiftTap_{};
    std::vector<std::string> strip_;
    int stripMain_ = 0;
};
