#include "ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

using Clock = std::chrono::steady_clock;

namespace {

void setRgb(cairo_t *cr, unsigned c, double a = 1.0) {
    cairo_set_source_rgba(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0, a);
}

void roundRect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void text(cairo_t *cr, const std::string &s, double cx, double cy, double size, bool bold, unsigned rgb) {
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents_t e;
    cairo_text_extents(cr, s.c_str(), &e);
    setRgb(cr, rgb);
    cairo_move_to(cr, cx - (e.width / 2 + e.x_bearing), cy - (e.height / 2 + e.y_bearing));
    cairo_show_text(cr, s.c_str());
}

void iconBackspace(cairo_t *cr, double cx, double cy, unsigned rgb) {
    setRgb(cr, rgb);
    cairo_set_line_width(cr, 2);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, cx - 12, cy); cairo_line_to(cr, cx - 5, cy - 8); cairo_line_to(cr, cx + 11, cy - 8);
    cairo_line_to(cr, cx + 11, cy + 8); cairo_line_to(cr, cx - 5, cy + 8); cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_move_to(cr, cx - 1, cy - 3.5); cairo_line_to(cr, cx + 7, cy + 3.5);
    cairo_move_to(cr, cx + 7, cy - 3.5); cairo_line_to(cr, cx - 1, cy + 3.5);
    cairo_stroke(cr);
}

void iconShift(cairo_t *cr, double cx, double cy, unsigned rgb, bool fill, bool lock) {
    setRgb(cr, rgb);
    cairo_set_line_width(cr, 2);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, cx, cy - 10); cairo_line_to(cr, cx + 10, cy + 1); cairo_line_to(cr, cx + 4.5, cy + 1);
    cairo_line_to(cr, cx + 4.5, cy + 9); cairo_line_to(cr, cx - 4.5, cy + 9); cairo_line_to(cr, cx - 4.5, cy + 1);
    cairo_line_to(cr, cx - 10, cy + 1); cairo_close_path(cr);
    if (fill) cairo_fill_preserve(cr);
    cairo_stroke(cr);
    if (lock) { cairo_move_to(cr, cx - 4.5, cy + 13); cairo_line_to(cr, cx + 4.5, cy + 13); cairo_stroke(cr); }
}

void iconEnter(cairo_t *cr, double cx, double cy, unsigned rgb) {
    setRgb(cr, rgb);
    cairo_set_line_width(cr, 2.2);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, cx + 10, cy - 8); cairo_line_to(cr, cx + 10, cy + 3); cairo_line_to(cr, cx - 9, cy + 3);
    cairo_stroke(cr);
    cairo_move_to(cr, cx - 4, cy - 2); cairo_line_to(cr, cx - 9, cy + 3); cairo_line_to(cr, cx - 4, cy + 8);
    cairo_stroke(cr);
}

std::vector<std::string> splitAlts(const std::string &s) {
    std::vector<std::string> r;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) r.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return r;
}

struct TKey { const char *label; unsigned code, mod; };
const TKey kTerm[] = {{"Esc", 1, 0}, {"Tab", 15, 0}, {"Ctrl", 0, 2}, {"Alt", 0, 4}, {"←", 105, 0}, {"↓", 108, 0}, {"↑", 103, 0}, {"→", 106, 0}};
constexpr int kTermN = 8;
struct PrefRow { const char *id, *label; };
const PrefRow kPrefs[] = {{"filters", "Filters"}, {"autocorrect", "Auto-correction"}, {"autocap", "Auto-capitalisation"},
                          {"key_preview", "Key preview"}, {"tall_keys", "Taller keys"}, {"light_theme", "Light theme"}, {"reduce_motion", "Reduce motion"}};
constexpr int kPrefN = 7;

}  // namespace

Ui::Ui(Engine &e, Lexicon &lex, SwipeDecoder &dec, Timers &t) : eng_(e), lex_(lex), dec_(dec), timers_(t) {
    eng_.onChange = [this] { onEngineChange(); };
}

Ui::~Ui() {
    if (animId_) { timers_.remove(animId_); animId_ = 0; }
    for (auto &[id, t] : touches_) { timers_.remove(t.lpTimer); timers_.remove(t.repTimer); }
    touches_.clear();
    if (base_) cairo_surface_destroy(base_);
}

void Ui::resize(int logicalW, int scale) {
    w_ = (float)logicalW; scale_ = std::max(1, scale);
    h_ = std::ceil(Layout::heightFor(w_));
    letters_.build(Page::Letters, w_, h_, ctx_);
    dec_.setLayout(letters_.centres(), letters_.keyW(), letters_.keyH());
    if (base_) cairo_surface_destroy(base_);
    base_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pxW(), pxH());
    rebuild();
}

void Ui::rebuild() {
    lay_.build(page_, w_, h_, ctx_);
    redrawAll();
}

void Ui::setContext(const Context &c) {
    ctx_ = c;
    term_ = c.terminal;
    mods_ = 0;
    page_ = c.numeric ? Page::Numpad : Page::Letters;
    if (w_ > 0) {
        letters_.build(Page::Letters, w_, h_, ctx_);
        rebuild();
    }
}

void Ui::reset() {
    if (animId_) { timers_.remove(animId_); animId_ = 0; }
    for (auto &[id, t] : touches_) { timers_.remove(t.lpTimer); timers_.remove(t.repTimer); }
    touches_.clear();
    fade_.clear(); lit_.clear();
    capture_ = false; captured_.clear(); modal_ = Modal::None; closing_ = false; modalT_ = 1.f;
    if (w_ > 0) redrawAll();
}

void Ui::invalidate(float x, float y, float w, float h) {
    PxRect r{(int)std::floor(x * scale_) - 2, (int)std::floor(y * scale_) - 2, (int)std::ceil((x + w) * scale_) + 2, (int)std::ceil((y + h) * scale_) + 2};
    r.x0 = std::max(0, r.x0); r.y0 = std::max(0, r.y0); r.x1 = std::min(pxW(), r.x1); r.y1 = std::min(pxH(), r.y1);
    dirty_.add(r);
}

PxRect Ui::takeDirty() { PxRect r = dirty_; dirty_ = {}; return r; }

std::string Ui::typed(const Key &k) const {
    std::string s = k.out;
    if (k.letter >= 0 && eng_.shift() != Engine::Off) for (char &c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

std::string Ui::labelFor(const Key &k) const { return k.type == KeyType::Char ? typed(k) : k.label; }

bool Ui::pressedKey(int i) const {
    for (auto &[id, t] : touches_) if (t.key == i && !t.swipe && !t.consumed) return true;
    return false;
}

void Ui::drawKey(cairo_t *cr, int i, bool pressed) {
    const Key &k = lay_.keys()[i];
    const double gx = 1.5, gy = 2.5;
    const double x = k.x + k.padL + gx, y = k.y + gy, w = k.w - k.padL - k.padR - 2 * gx, h = k.h - 2 * gy;
    unsigned fill = k.accent ? th_.accent : (k.type == KeyType::Char || k.type == KeyType::Space) ? th_.key : th_.special;
    if (page_ == Page::Letters && k.type == KeyType::Char && k.label.size() == 1 && std::isdigit((unsigned char)k.label[0])) fill = th_.special;
    const float amt = pressed ? 1.f : keyAmount(i);
    if (amt > 0) {
        const unsigned to = k.accent ? 0xc2d7fa : th_.pressed;
        auto mix = [&](int sh) { return (unsigned)(((fill >> sh) & 255) + (((to >> sh) & 255) - (int)((fill >> sh) & 255)) * amt) & 255; };
        fill = (mix(16) << 16) | (mix(8) << 8) | mix(0);
    }
    // clear the cell first: base keys are redrawn in place
    cairo_save(cr);
    cairo_rectangle(cr, k.x, k.y, k.w, k.h);
    cairo_clip(cr);
    setRgb(cr, th_.bg);
    cairo_paint(cr);
    setRgb(cr, fill);
    roundRect(cr, x, y, w, h, 5);
    cairo_fill(cr);
    const double cx = x + w / 2, cy = y + h / 2;
    switch (k.type) {
    case KeyType::Backspace: iconBackspace(cr, cx, cy, th_.text); break;
    case KeyType::Shift: {
        const bool on = eng_.shift() != Engine::Off;
        iconShift(cr, cx, cy - (eng_.shift() == Engine::Lock ? 1 : 0), eng_.shift() == Engine::Lock ? th_.accent : th_.text, on, eng_.shift() == Engine::Lock);
        break;
    }
    case KeyType::Enter: iconEnter(cr, cx, cy, th_.onAccent); break;
    case KeyType::Space: break;
    default: {
        const std::string l = labelFor(k);
        const bool digit = page_ == Page::Letters && l.size() == 1 && std::isdigit((unsigned char)l[0]);
        const double size = k.small ? 15 : digit ? 19 : (l.size() > 2 ? 17 : 24);
        text(cr, l, cx, cy - (k.small ? 0 : 1), size, false, digit ? th_.dim : th_.text);
    }
    }
    cairo_restore(cr);
}

void Ui::drawStripInto(cairo_t *cr) {
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, w_, Layout::kStrip);
    cairo_clip(cr);
    setRgb(cr, th_.bg);
    cairo_paint(cr);
    const float sw = (w_ - kHide) / 3;
    const int n = (term_ || capture_) ? 0 : (int)std::min<size_t>(3, strip_.size());
    if (capture_) {
        text(cr, std::string("Block word: ") + captured_ + "|", (w_ - kHide) / 2, Layout::kStrip / 2, 19, true, th_.accent);
    }
    if (term_ && !capture_) {
        const float tw = (w_ - kHide) / kTermN;
        for (int i = 0; i < kTermN; ++i) {
            const bool on = kTerm[i].mod && (mods_ & kTerm[i].mod);
            setRgb(cr, on ? th_.accent : th_.key);
            roundRect(cr, tw * i + 2, 4, tw - 4, Layout::kStrip - 8, 5);
            cairo_fill(cr);
            const unsigned code = kTerm[i].code;
            if (code == 105 || code == 106 || code == 103 || code == 108) {
                const double cx = tw * i + tw / 2, cy = Layout::kStrip / 2;
                const double dx = code == 105 ? -1 : code == 106 ? 1 : 0, dy = code == 103 ? -1 : code == 108 ? 1 : 0;
                setRgb(cr, th_.text);
                cairo_set_line_width(cr, 2.4); cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND); cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                cairo_move_to(cr, cx - dx * 5 - dy * 7 + (dy ? 0 : 0), cy - dy * 5 - dx * 7);
                cairo_line_to(cr, cx + dx * 5, cy + dy * 5);
                cairo_line_to(cr, cx - dx * 5 + dy * 7, cy - dy * 5 + dx * 7);
                cairo_stroke(cr);
            } else
            text(cr, kTerm[i].label, tw * i + tw / 2, Layout::kStrip / 2, 16, false, on ? th_.onAccent : th_.text);
        }
    }
    for (int i = 0; i < n; ++i) {
        std::string s = strip_[i];
        text(cr, s, sw * i + sw / 2, Layout::kStrip / 2, 19, i == stripMain_ && n > 1, i == stripMain_ ? th_.text : th_.dim);
        if (i < n - 1 || n == 3) {
            if (i < 2) { setRgb(cr, th_.key); cairo_rectangle(cr, sw * (i + 1) - 0.5, 11, 1, Layout::kStrip - 22); cairo_fill(cr); }
        }
    }
    {   // hide button: chevron down
        const double cx = w_ - kHide / 2, cy = Layout::kStrip / 2;
        setRgb(cr, th_.dim);
        cairo_set_line_width(cr, 2.4); cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND); cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_move_to(cr, cx - 9, cy - 4); cairo_line_to(cr, cx, cy + 5); cairo_line_to(cr, cx + 9, cy - 4);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

void Ui::redrawAll() {
    if (!base_) return;
    cairo_t *cr = cairo_create(base_);
    cairo_scale(cr, scale_, scale_);
    setRgb(cr, th_.bg);
    cairo_paint(cr);
    drawStripInto(cr);
    for (int i = 0; i < (int)lay_.keys().size(); ++i) drawKey(cr, i, pressedKey(i));
    cairo_destroy(cr);
    lastShift_ = eng_.shift();
    invalidateAll();
}

float Ui::keyAmount(int i) const {
    auto it = fade_.find(i);
    return it == fade_.end() ? 0.f : it->second;
}

void Ui::redrawKey(int i) {
    if (!base_ || i < 0 || i >= (int)lay_.keys().size()) return;
    if (pressedKey(i)) { lit_.insert(i); fade_.erase(i); }
    else if (lit_.erase(i) && !reduced()) { fade_[i] = 1.f; startAnim(); }
    cairo_t *cr = cairo_create(base_);
    cairo_scale(cr, scale_, scale_);
    drawKey(cr, i, pressedKey(i));
    cairo_destroy(cr);
    const Key &k = lay_.keys()[i];
    invalidate(k.x, k.y, k.w, k.h);
}

void Ui::redrawStrip() {
    if (!base_) return;
    cairo_t *cr = cairo_create(base_);
    cairo_scale(cr, scale_, scale_);
    drawStripInto(cr);
    cairo_destroy(cr);
    invalidate(0, 0, w_, Layout::kStrip);
}

void Ui::onEngineChange() {
    if (!base_) return;
    if (eng_.shift() != lastShift_) { lastShift_ = eng_.shift(); redrawAll(); }
    if (strip_ != eng_.suggestions() || stripMain_ != eng_.mainSuggestion()) {
        strip_ = eng_.suggestions();
        stripMain_ = eng_.mainSuggestion();
        redrawStrip();
    }
}

// ---- overlays: key preview, long-press alternates, swipe trail

void Ui::altGeometry(const Touch &t, float &x, float &y, float &cw, float &ch, std::vector<std::string> &alts) const {
    const Key &k = lay_.keys()[t.key];
    alts = splitAlts(k.alts);
    if (eng_.shift() != Engine::Off && k.letter >= 0)
        for (auto &a : alts) {   // upper-case alternates (ASCII only; accented letters keep their case)
            if (a.size() == 1) a[0] = (char)std::toupper((unsigned char)a[0]);
        }
    cw = std::max(lay_.keyW() * 0.92f, 38.f);
    ch = lay_.keyH() * 1.05f;
    const float total = cw * alts.size();
    x = std::min(std::max(2.f, k.x + k.w / 2 - total / 2), w_ - total - 2);
    y = std::max(0.f, k.y - ch - 4);
}

void Ui::drawOverlays(cairo_t *cr, const PxRect &) {
    for (auto &[id, t] : touches_) {
        if (t.key < 0 || t.consumed) continue;
        const Key &k = lay_.keys()[t.key];
        if (t.longOpen) {
            float x, y, cw, ch; std::vector<std::string> alts;
            altGeometry(t, x, y, cw, ch, alts);
            setRgb(cr, th_.popup);
            roundRect(cr, x - 3, y - 3, cw * alts.size() + 6, ch + 6, 11);
            cairo_fill(cr);
            for (size_t i = 0; i < alts.size(); ++i) {
                if ((int)i == t.altSel) { setRgb(cr, th_.accent); roundRect(cr, x + cw * i + 2, y, cw - 4, ch, 8); cairo_fill(cr); }
                text(cr, alts[i], x + cw * i + cw / 2, y + ch / 2, 26, false, (int)i == t.altSel ? th_.onAccent : th_.text);
            }
        } else if (!t.swipe && k.type == KeyType::Char && t.key >= 0 && (!set_ || set_->preview)) {
            const float bw = lay_.keyW() * 1.3f, bh = lay_.keyH() * 1.3f;
            const float bx = std::min(std::max(2.f, k.x + k.w / 2 - bw / 2), w_ - bw - 2), by = std::max(0.f, k.y - bh + 8);
            setRgb(cr, th_.popup);
            roundRect(cr, bx, by, bw, bh, 12);
            cairo_fill(cr);
            text(cr, labelFor(k), bx + bw / 2, by + bh / 2 - 4, 34, false, th_.text);
        }
    }
    for (auto &[id, t] : touches_) {
        if (!t.swipe || t.path.size() < 2) continue;
        const size_t n = t.path.size(), from = n > 56 ? n - 56 : 0;
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        for (size_t i = from + 1; i < n; ++i) {
            const double f = double(i - from) / double(n - from);
            setRgb(cr, th_.accent, 0.25 + 0.7 * f * f);
            cairo_set_line_width(cr, 2.5 + 5.0 * f);
            cairo_move_to(cr, t.path[i - 1].x, t.path[i - 1].y);
            cairo_line_to(cr, t.path[i].x, t.path[i].y);
            cairo_stroke(cr);
        }
    }
}

Theme Ui::lightTheme() {
    Theme t;
    t.bg = 0xe8eaed; t.key = 0xffffff; t.special = 0xdadce0; t.pressed = 0xbdc1c6; t.text = 0x202124; t.dim = 0x5f6368;
    t.accent = 0x1a73e8; t.onAccent = 0xffffff; t.popup = 0xffffff; t.dialog = 0xffffff; t.sel = 0xd3e3fd;
    return t;
}

bool Ui::getFlag(int i) const {
    if (!set_) return false;
    switch (i) { case 1: return set_->autocorrect; case 2: return set_->autocap;
                 case 3: return set_->preview; case 4: return set_->tall; case 5: return set_->light; default: return set_->reduceMotion; }
}

void Ui::setFlag(int i) {
    if (!set_) return;
    switch (i) { case 1: set_->autocorrect = !set_->autocorrect; break;
                 case 2: set_->autocap = !set_->autocap; break; case 3: set_->preview = !set_->preview; break;
                 case 4: set_->tall = !set_->tall; break; case 5: set_->light = !set_->light; break; default: set_->reduceMotion = !set_->reduceMotion; break; }
    if (onSettingChanged) onSettingChanged(kPrefs[i].id);
}

void Ui::openModal(Modal m) {
    const bool was = modal_ != Modal::None;
    modal_ = m;
    closing_ = false;
    std::vector<int> keys;
    for (auto &[id, t] : touches_) {
        timers_.remove(t.lpTimer); timers_.remove(t.repTimer);
        if (t.key >= 0) keys.push_back(t.key);
    }
    touches_.clear();
    for (int k : keys) redrawKey(k);
    invalidateAll();
    modalDirty_ = true;
    if (!was) {
        modalT_ = (reduced() || !onModalVisible) ? 1.f : 0.f;
        if (modalT_ < 1.f) startAnim();
        if (onModalVisible) onModalVisible(true);
    } else if (modalT_ < 1.f) startAnim();
}

void Ui::finishClose() {
    modal_ = Modal::None;
    closing_ = false;
    modalT_ = 1.f;
    invalidateAll();
    if (onModalVisible) onModalVisible(false);
}

void Ui::closeModalNow() {
    if (modal_ == Modal::None) return;
    finishClose();
}

void Ui::closeModal() {
    if (modal_ == Modal::None || closing_) return;
    if (reduced() || !onModalVisible) { finishClose(); return; }
    closing_ = true;
    startAnim();
}

void Ui::startAnim() {
    if (animId_) return;
    animId_ = timers_.add(16, [this] { return animTick(); });
}

bool Ui::animTick() {
    bool more = false;
    if (modal_ != Modal::None) {
        const float step = closing_ ? -16.f / 120.f : 16.f / 180.f;
        modalT_ = std::min(1.f, std::max(0.f, modalT_ + step));
        modalDirty_ = true;
        if (closing_ && modalT_ <= 0.f) finishClose();
        else if (!closing_ && modalT_ >= 1.f) {}
        else more = true;
    }
    for (auto it = fade_.begin(); it != fade_.end();) {
        const int i = it->first;
        it->second -= 16.f / 150.f;
        if (it->second <= 0.f) { it = fade_.erase(it); redrawKey(i); }
        else { const int j = i; ++it; redrawKey(j); more = true; }
    }
    if (!more) animId_ = 0;
    return more;
}

// rows: Langs -> id 0..n-1 language, 100 footer link; Prefs -> id 0..5 toggles, 200 done; id -1 = dialog body
void Ui::layoutModal(std::vector<Row> &rows, float &dx, float &dy, float &dw, float &dh) const {
    rows.clear();
    const float cw = mw_ > 0 ? mw_ : w_, ch = mh_ > 0 ? mh_ : h_;
    dw = std::min(cw - 24.f, 380.f);
    dx = (cw - dw) / 2;
    const float title = 46;
    if (modal_ == Modal::Langs) {
        const float rh = std::min(56.f, (ch - 12 - title - 44) / std::max<size_t>(1, langs_.size()));
        dh = title + rh * langs_.size() + 44;
        dy = std::max(4.f, (ch - dh) / 2);
        for (size_t i = 0; i < langs_.size(); ++i) rows.push_back({dx + 12, dy + title + rh * i, dw - 24, rh, (int)i});
        rows.push_back({dx + dw - 190, dy + dh - 44, 178, 44, 100});
    } else if (modal_ == Modal::Filters) {
        const int n = (int)filters_.size() + 1;
        const float rh = std::max(30.f, std::min(44.f, (ch - 12 - title) / n));
        dh = title + rh * n + 6;
        dy = std::max(4.f, (ch - dh) / 2);
        for (size_t i = 0; i < filters_.size(); ++i) rows.push_back({dx, dy + title + rh * i, dw, rh, (int)i});
        rows.push_back({dx, dy + title + rh * filters_.size(), dw, rh, 300});
        rows.push_back({dx + dw - 90, dy, 90, title, 200});
    } else {
        const float rh = std::min(44.f, (ch - 12 - title) / kPrefN);
        dh = title + rh * kPrefN + 6;
        dy = std::max(4.f, (ch - dh) / 2);
        for (int i = 0; i < kPrefN; ++i) rows.push_back({dx, dy + title + rh * i, dw, rh, i});
        rows.push_back({dx + dw - 90, dy, 90, title, 200});
    }
}

void Ui::drawModal(cairo_t *cr) {
    if (modal_ == Modal::None) return;
    const float e = 1.f - std::pow(1.f - modalT_, 3.f);
    setRgb(cr, 0x000000, 0.5 * e);
    cairo_rectangle(cr, 0, 0, mw_ > 0 ? mw_ : w_, mh_ > 0 ? mh_ : h_);
    cairo_fill(cr);
    std::vector<Row> rows; float dx, dy, dw, dh;
    layoutModal(rows, dx, dy, dw, dh);
    cairo_save(cr);
    if (e < 1.f) {
        const float sc = 0.9f + 0.1f * e;
        cairo_translate(cr, dx + dw / 2, dy + dh / 2 + (1.f - e) * 14);
        cairo_scale(cr, sc, sc);
        cairo_translate(cr, -(dx + dw / 2), -(dy + dh / 2));
        cairo_push_group(cr);
    }
    setRgb(cr, th_.dialog);
    roundRect(cr, dx, dy, dw, dh, 20);
    cairo_fill(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    auto left = [&](const std::string &s, double x, double cy, double size, unsigned rgb) {
        cairo_set_font_size(cr, size);
        cairo_text_extents_t e; cairo_text_extents(cr, s.c_str(), &e);
        setRgb(cr, rgb);
        cairo_move_to(cr, x, cy - (e.height / 2 + e.y_bearing));
        cairo_show_text(cr, s.c_str());
    };
    auto right = [&](const std::string &s, double xr, double cy, double size, unsigned rgb) {
        cairo_set_font_size(cr, size);
        cairo_text_extents_t e; cairo_text_extents(cr, s.c_str(), &e);
        setRgb(cr, rgb);
        cairo_move_to(cr, xr - e.x_advance, cy - (e.height / 2 + e.y_bearing));
        cairo_show_text(cr, s.c_str());
    };
    if (modal_ == Modal::Langs) {
        left("Change keyboard", dx + 24, dy + 23, 20, th_.text);
        for (const Row &r : rows) {
            if (r.id == 100) { right("Keyboard settings", r.x + r.w, r.y + r.h / 2, 15, th_.accent); continue; }
            const Lang &l = langs_[r.id];
            const bool cur = set_ && set_->lang == l.code;
            if (cur) { setRgb(cr, th_.sel); roundRect(cr, r.x, r.y + 2, r.w, r.h - 4, (r.h - 4) / 2); cairo_fill(cr); }
            left(l.label, r.x + 54, r.y + r.h / 2 - 8, 17, th_.text);
            left(l.sub, r.x + 54, r.y + r.h / 2 + 12, 12, th_.dim);
            if (cur) {
                setRgb(cr, th_.accent); cairo_set_line_width(cr, 2.6); cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND); cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                const double cx = r.x + 26, cy = r.y + r.h / 2;
                cairo_move_to(cr, cx - 7, cy); cairo_line_to(cr, cx - 2, cy + 5); cairo_line_to(cr, cx + 7, cy - 5); cairo_stroke(cr);
            }
        }
    } else if (modal_ == Modal::Filters) {
        left("Filters", dx + 24, dy + 23, 20, th_.text);
        for (const Row &r : rows) {
            if (r.id == 200) { right("Back", r.x + r.w - 20, r.y + 23, 16, th_.accent); continue; }
            if (r.id == 300) { left("Block a word…", r.x + 24, r.y + r.h / 2, 16, th_.accent); continue; }
            left(filters_[r.id].label, r.x + 24, r.y + r.h / 2, 16, th_.text);
            const bool on = set_ && set_->filters.count(filters_[r.id].id) && set_->filters.at(filters_[r.id].id);
            const double sw = 42, sh = 22, sx = r.x + r.w - 24 - sw, sy = r.y + r.h / 2 - sh / 2;
            setRgb(cr, on ? th_.accent : th_.pressed); roundRect(cr, sx, sy, sw, sh, sh / 2); cairo_fill(cr);
            setRgb(cr, on ? th_.onAccent : th_.text);
            cairo_arc(cr, on ? sx + sw - sh / 2 : sx + sh / 2, sy + sh / 2, sh / 2 - 3.5, 0, 2 * M_PI); cairo_fill(cr);
        }
    } else {
        left("Keyboard settings", dx + 24, dy + 23, 20, th_.text);
        for (const Row &r : rows) {
            if (r.id == 200) { right("Done", r.x + r.w - 20, r.y + 23, 16, th_.accent); continue; }
            left(kPrefs[r.id].label, r.x + 24, r.y + r.h / 2, 16, th_.text);
            if (r.id == 0) {   // submenu
                setRgb(cr, th_.dim); cairo_set_line_width(cr, 2.2); cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND); cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                const double cx = r.x + r.w - 30, cy = r.y + r.h / 2;
                cairo_move_to(cr, cx - 4, cy - 7); cairo_line_to(cr, cx + 3, cy); cairo_line_to(cr, cx - 4, cy + 7); cairo_stroke(cr);
                continue;
            }
            const bool on = getFlag(r.id);
            const double sw = 42, sh = 22, sx = r.x + r.w - 24 - sw, sy = r.y + r.h / 2 - sh / 2;
            setRgb(cr, on ? th_.accent : th_.pressed); roundRect(cr, sx, sy, sw, sh, sh / 2); cairo_fill(cr);
            setRgb(cr, on ? th_.onAccent : th_.text);
            cairo_arc(cr, on ? sx + sw - sh / 2 : sx + sh / 2, sy + sh / 2, sh / 2 - 3.5, 0, 2 * M_PI); cairo_fill(cr);
        }
    }
    if (e < 1.f) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, e);
    }
    cairo_restore(cr);
}

void Ui::finishCapture(bool save) {
    if (!capture_) return;
    capture_ = false;
    const std::string w = captured_;
    captured_.clear();
    redrawStrip();
    if (save && w.size() >= 2 && onBlockWord) onBlockWord(w);
}

void Ui::bsKey() {
    if (capture_) { if (!captured_.empty()) captured_.pop_back(); redrawStrip(); }
    else eng_.backspace();
}

void Ui::modalTap(float x, float y) {
    std::vector<Row> rows; float dx, dy, dw, dh;
    layoutModal(rows, dx, dy, dw, dh);
    if (closing_) return;
    if (x < dx || x > dx + dw || y < dy || y > dy + dh) { closeModal(); return; }
    for (const Row &r : rows) {
        if (x < r.x || x > r.x + r.w || y < r.y || y > r.y + r.h) continue;
        if (modal_ == Modal::Langs) {
            if (r.id == 100) { openModal(Modal::Prefs); return; }
            if (set_ && set_->lang != langs_[r.id].code) { set_->lang = langs_[r.id].code; if (onSettingChanged) onSettingChanged("language"); }
            closeModal();
        } else if (modal_ == Modal::Filters) {
            if (r.id == 200) { openModal(Modal::Prefs); return; }
            if (r.id == 300) { closeModal(); capture_ = true; captured_.clear(); redrawStrip(); return; }
            const std::string &fid = filters_[r.id].id;
            if (set_) { bool &e = set_->filters[fid]; e = !e; }
            if (onSettingChanged) onSettingChanged("filters");
            modalDirty_ = true;
        } else {
            if (r.id == 200) { closeModal(); return; }
            if (r.id == 0) { openModal(Modal::Filters); return; }
            setFlag(r.id);
            modalDirty_ = true;
        }
        return;
    }
}

void Ui::blit(unsigned char *dst, int stride, const PxRect &r) {
    if (!base_ || r.empty()) return;
    cairo_surface_flush(base_);
    const unsigned char *src = cairo_image_surface_get_data(base_);
    const int sstride = cairo_image_surface_get_stride(base_);
    for (int y = r.y0; y < r.y1; ++y) std::memcpy(dst + (size_t)y * stride + r.x0 * 4, src + (size_t)y * sstride + r.x0 * 4, (size_t)(r.x1 - r.x0) * 4);
    cairo_surface_t *s = cairo_image_surface_create_for_data(dst, CAIRO_FORMAT_ARGB32, pxW(), pxH(), stride);
    cairo_t *cr = cairo_create(s);
    cairo_rectangle(cr, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
    cairo_clip(cr);
    cairo_scale(cr, scale_, scale_);
    drawOverlays(cr, r);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    cairo_surface_destroy(s);
}

void Ui::blitModal(unsigned char *dst, int stride) {
    const int pw = (int)((mw_ > 0 ? mw_ : w_) * scale_), ph = (int)((mh_ > 0 ? mh_ : h_) * scale_);
    cairo_surface_t *s = cairo_image_surface_create_for_data(dst, CAIRO_FORMAT_ARGB32, pw, ph, stride);
    cairo_t *cr = cairo_create(s);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, scale_, scale_);
    drawModal(cr);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    cairo_surface_destroy(s);
}

bool Ui::savePng(const char *path) {
    // test snapshot: keyboard at the bottom of a phone-sized canvas, dialog full-screen above it
    const float fullH = w_ * 2.0f;
    const bool m = modal_ != Modal::None;
    const int W = pxW(), H = m ? (int)(fullH * scale_) : pxH();
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    PxRect all{0, 0, pxW(), pxH()};
    cairo_surface_t *kb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pxW(), pxH());
    blit(cairo_image_surface_get_data(kb), cairo_image_surface_get_stride(kb), all);
    cairo_surface_mark_dirty(kb);
    cairo_t *cr = cairo_create(s);
    cairo_set_source_rgb(cr, 0.08, 0.09, 0.12);
    cairo_paint(cr);
    cairo_set_source_surface(cr, kb, 0, H - pxH());
    cairo_paint(cr);
    if (m) {
        const float ow = mw_, oh = mh_;
        mw_ = w_; mh_ = fullH;
        cairo_save(cr);
        cairo_scale(cr, scale_, scale_);
        drawModal(cr);
        cairo_restore(cr);
        mw_ = ow; mh_ = oh;
    }
    cairo_destroy(cr);
    const bool ok = cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(kb);
    cairo_surface_destroy(s);
    return ok;
}

void Ui::invalidateTrail() {
    PxRect box;
    for (auto &[id, t] : touches_) {
        if (!t.swipe || t.path.empty()) continue;
        const size_t n = t.path.size(), from = n > 56 ? n - 56 : 0;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (size_t i = from; i < n; ++i) { x0 = std::min(x0, t.path[i].x); y0 = std::min(y0, t.path[i].y); x1 = std::max(x1, t.path[i].x); y1 = std::max(y1, t.path[i].y); }
        PxRect r{(int)std::floor((x0 - 6) * scale_), (int)std::floor((y0 - 6) * scale_), (int)std::ceil((x1 + 6) * scale_), (int)std::ceil((y1 + 6) * scale_)};
        box.add(r);
    }
    PxRect all = trailBox_;
    all.add(box);
    all.x0 = std::max(0, all.x0); all.y0 = std::max(0, all.y0); all.x1 = std::min(pxW(), all.x1); all.y1 = std::min(pxH(), all.y1);
    dirty_.add(all);
    trailBox_ = box;
}

// ---- touch

int Ui::pickKey(float x, float y) const {
    const int base = lay_.keyAt(x, y);
    if (base < 0 || page_ != Page::Letters || y < Layout::kStrip) return base;
    const auto &ks = lay_.keys();
    if (ks[base].letter < 0) return base;
    std::string pre;
    for (char c : eng_.composing()) if (c != '\'') pre += (char)std::tolower((unsigned char)c);
    float pr[26];
    bool have = !pre.empty() && pre.size() < 20 && lex_.nextLetters(pre, pr);
    if (!have) return base;
    int best = base; float bc = 1e9f;
    for (int i = 0; i < (int)ks.size(); ++i) {
        const Key &k = ks[i];
        if (k.letter < 0) continue;
        const float kx = k.x + k.padL + (k.w - k.padL - k.padR) / 2;
        const float dx = (x - kx) / lay_.keyW(), dy = (y - (k.y + k.h / 2)) / lay_.keyH();
        const float d2 = dx * dx + dy * dy;
        if (d2 > 1.3f) continue;
        const float c = d2 - 0.22f * std::log(pr[k.letter] + 0.01f);
        if (c < bc) { bc = c; best = i; }
    }
    return best;
}

void Ui::startLongPress(Touch &t) {
    const int id = t.id;
    t.lpTimer = timers_.add(380, [this, id] {
        auto it = touches_.find(id);
        if (it == touches_.end()) return false;
        it->second.lpTimer = 0;
        openAlts(it->second);
        return false;
    });
}

void Ui::openAlts(Touch &t) {
    if (capture_ || t.swipe || t.key < 0 || t.consumed) return;
    const Key &k = lay_.keys()[t.key];
    if (k.alts.empty()) return;
    t.longOpen = true;
    float x, y, cw, ch; std::vector<std::string> alts;
    altGeometry(t, x, y, cw, ch, alts);
    t.altSel = std::min<int>((int)alts.size() - 1, std::max(0, (int)((t.x - x) / cw)));
    invalidate(x - 4, y - 4, cw * alts.size() + 8, ch + 8);
    invalidate(k.x, k.y - 90, k.w, 100);
}

void Ui::touchDown(int id, float x, float y) {
    if (modal_ != Modal::None) return;
    Touch t;
    t.id = id; t.x0 = t.x = x; t.y0 = t.y = y; t.cursorX = x;
    if (y < Layout::kStrip) {
        t.slot = x >= w_ - kHide ? 100 : term_ ? std::min(kTermN - 1, std::max(0, (int)(x / ((w_ - kHide) / kTermN)))) : std::min(2, std::max(0, (int)(x / ((w_ - kHide) / 3))));
        touches_[id] = t;
        return;
    }
    t.key = lay_.keyAt(x, y);
    if (t.key < 0) { touches_[id] = t; return; }
    const Key &k = lay_.keys()[t.key];
    touches_[id] = t;
    Touch &tt = touches_[id];
    if (k.type == KeyType::Char && page_ == Page::Letters && k.letter >= 0) {
        tt.path.push_back({x, y});
        tt.stamp.push_back(Clock::now());
    }
    redrawKey(tt.key);
    if (k.type == KeyType::Char) {
        invalidate(k.x - lay_.keyW() * 0.5f, k.y - lay_.keyH() * 1.4f, k.w + lay_.keyW(), lay_.keyH() * 1.5f);
        if (!k.alts.empty()) startLongPress(tt);
    } else if (k.type == KeyType::Space) {
        tt.lpTimer = timers_.add(450, [this, id] {
            auto it = touches_.find(id);
            if (it == touches_.end()) return false;
            it->second.lpTimer = 0;
            it->second.opened = true;
            openModal(Modal::Langs);
            return false;
        });
    } else if (k.type == KeyType::Backspace) {
        bsKey();
        tt.consumed = false;
        tt.repTimer = timers_.add(420, [this, id] {
            auto it = touches_.find(id);
            if (it == touches_.end()) return false;
            it->second.repeating = true;
            it->second.repTimer = timers_.add(45, [this, id] {
                auto j = touches_.find(id);
                if (j == touches_.end()) return false;
                bsKey();
                return true;
            });
            return false;
        });
    }
}

void Ui::touchMotion(int id, float x, float y) {
    if (modal_ != Modal::None) return;
    auto it = touches_.find(id);
    if (it == touches_.end()) return;
    Touch &t = it->second;
    t.x = x; t.y = y;
    if (t.key < 0) return;
    const Key &k = lay_.keys()[t.key];
    if (t.longOpen) {
        float ax, ay, cw, ch; std::vector<std::string> alts;
        altGeometry(t, ax, ay, cw, ch, alts);
        const int sel = std::min<int>((int)alts.size() - 1, std::max(0, (int)((x - ax) / cw)));
        if (sel != t.altSel) { t.altSel = sel; invalidate(ax - 4, ay - 4, cw * alts.size() + 8, ch + 8); }
        return;
    }
    if (!t.path.empty()) {
        t.path.push_back({x, y});
        t.stamp.push_back(Clock::now());
        if (!t.swipe) {
            const float d = std::hypot(x - t.x0, y - t.y0);
            if (d > lay_.keyW() * 0.55f) {
                t.swipe = true;
                timers_.remove(t.lpTimer); t.lpTimer = 0;
                const int kk = t.key;
                redrawKey(kk);                // key un-highlights, preview goes
                invalidate(k.x - lay_.keyW() * 0.5f, k.y - lay_.keyH() * 1.4f, k.w + lay_.keyW(), lay_.keyH() * 1.5f);
            }
        }
        if (t.swipe) invalidateTrail();
        return;
    }
    if (k.type == KeyType::Space) {
        if (!t.spaceDrag && std::fabs(x - t.x0) > 14) { t.spaceDrag = true; t.cursorX = x; timers_.remove(t.lpTimer); t.lpTimer = 0; }
        if (t.spaceDrag) {
            while (x - t.cursorX > 15) { eng_.moveCursor(1); t.cursorX += 15; }
            while (t.cursorX - x > 15) { eng_.moveCursor(-1); t.cursorX -= 15; }
        }
        return;
    }
    if (k.type == KeyType::Char) {       // non-letter pages: the key under the finger at release wins
        const int nk = lay_.keyAt(x, y);
        if (nk >= 0 && nk != t.key && lay_.keys()[nk].type == KeyType::Char) {
            const int old = t.key;
            invalidate(k.x - lay_.keyW() * 0.5f, k.y - lay_.keyH() * 1.4f, k.w + lay_.keyW(), lay_.keyH() * 1.5f);
            t.key = nk;
            redrawKey(old); redrawKey(nk);
            const Key &n = lay_.keys()[nk];
            invalidate(n.x - lay_.keyW() * 0.5f, n.y - lay_.keyH() * 1.4f, n.w + lay_.keyW(), lay_.keyH() * 1.5f);
        }
    }
}

void Ui::swipeEnd(Touch &t) {
    if (capture_) return;
    std::vector<Pt> path = t.path;
    eng_.swipe(path);
}

void Ui::release(Touch &t) {
    timers_.remove(t.lpTimer); timers_.remove(t.repTimer);
    t.lpTimer = t.repTimer = 0;
    if (t.key < 0) {
        if (t.slot >= 0 && std::fabs(t.x - t.x0) < 20 && t.y < Layout::kStrip + 10) {
            if (t.slot == 100) { if (capture_) finishCapture(false); else if (onHide) onHide(); }
            else if (capture_) {}
            else if (term_) {
                const TKey &tk = kTerm[t.slot];
                if (tk.mod) { mods_ ^= tk.mod; redrawStrip(); }
                else { eng_.pressKey(tk.code, mods_); if (mods_) { mods_ = 0; redrawStrip(); } }
            } else eng_.pickSuggestion(t.slot);
        }
        return;
    }
    Key k = lay_.keys()[t.key];
    if (t.swipe) { swipeEnd(t); return; }
    if (t.longOpen) {
        float x, y, cw, ch; std::vector<std::string> alts;
        altGeometry(t, x, y, cw, ch, alts);
        if (t.altSel >= 0 && t.altSel < (int)alts.size()) eng_.typeChar(alts[t.altSel]);
        return;
    }
    // a tap that drifted: letters/chars take the key under the finger at release
    if (k.type == KeyType::Char && t.y >= Layout::kStrip) {
        const int nk = pickKey(t.x, t.y);
        if (nk >= 0 && lay_.keys()[nk].type == KeyType::Char) k = lay_.keys()[nk];
    }
    switch (k.type) {
    case KeyType::Char:
        if (capture_) { const std::string o = k.out; if (o.size() == 1 && (std::isalpha((unsigned char)o[0]) || o[0] == '\'')) { captured_ += (char)std::tolower((unsigned char)o[0]); redrawStrip(); } break; }
        if (term_ && mods_) { eng_.combo(k.out, mods_); mods_ = 0; redrawStrip(); }
        else eng_.typeChar(typed(k));
        break;
    case KeyType::Space: if (!t.spaceDrag && !t.opened && !capture_) eng_.space(); break;
    case KeyType::Enter: if (capture_) finishCapture(true); else eng_.enter(); break;
    case KeyType::Shift: {
        const auto now = Clock::now();
        if (now - lastShiftTap_ < std::chrono::milliseconds(350)) { eng_.doubleTapShift(); lastShiftTap_ = {}; }
        else { eng_.tapShift(); lastShiftTap_ = now; }
        break;
    }
    case KeyType::Page: page_ = k.target; rebuild(); break;
    case KeyType::Backspace: break;
    }
}

void Ui::touchUp(int id) {
    if (modal_ != Modal::None) return;
    auto it = touches_.find(id);
    if (it == touches_.end()) return;
    Touch t = it->second;
    touches_.erase(it);
    const int key = t.key;
    release(t);
    if (t.swipe) invalidateTrail();
    if (key >= 0 && key < (int)lay_.keys().size()) {
        redrawKey(key);
        const Key &k = lay_.keys()[key];
        invalidate(k.x - lay_.keyW() * 0.6f, k.y - lay_.keyH() * 1.5f, k.w + lay_.keyW() * 1.2f, lay_.keyH() * 1.6f);
    }
    if (t.longOpen) invalidateAll();
}

void Ui::touchCancel(int id) {
    auto it = touches_.find(id);
    if (it == touches_.end()) return;
    Touch t = it->second;
    timers_.remove(t.lpTimer); timers_.remove(t.repTimer);
    touches_.erase(it);
    if (t.swipe) invalidateTrail();
    if (t.key >= 0) redrawKey(t.key);
    invalidateAll();
}

void Ui::touchCancelAll() {
    std::vector<int> ids;
    ids.reserve(touches_.size());
    for (const auto &kv : touches_) ids.push_back(kv.first);
    for (int id : ids) touchCancel(id);
}

bool Ui::keyPos(const std::string &label, float &x, float &y) const {
    if (term_) {
        const float tw = (w_ - kHide) / kTermN;
        for (int i = 0; i < kTermN; ++i)
            if (label == kTerm[i].label || (i >= 4 && label == std::string("arrow") + std::to_string(i - 4))) { x = tw * i + tw / 2; y = Layout::kStrip / 2; return true; }
    }
    if (label == "hide") { x = w_ - kHide / 2; y = Layout::kStrip / 2; return true; }
    if (label.rfind("sug", 0) == 0) { const int i = std::atoi(label.c_str() + 3); x = (w_ - kHide) / 3 * (i + 0.5f); y = Layout::kStrip / 2; return true; }
    for (const Key &k : lay_.keys()) {
        const bool m = (k.type == KeyType::Char && labelFor(k) == label) || (k.type == KeyType::Space && label == "space") ||
                       (k.type != KeyType::Char && k.type != KeyType::Space && k.label == label);
        if (m) { x = k.x + k.padL + (k.w - k.padL - k.padR) / 2; y = k.y + k.h / 2; return true; }
    }
    return false;
}

std::vector<Pt> Ui::wordPath(const std::string &word) const {
    std::vector<Pt> pts;
    const auto &c = letters_.centres();
    std::string w;
    for (char ch : word) if (w.empty() || w.back() != ch) w += ch;
    for (size_t i = 0; i < w.size(); ++i) {
        const Pt b = c[w[i] - 'a'];
        if (i == 0) { pts.push_back(b); continue; }
        const Pt a = c[w[i - 1] - 'a'];
        for (int s = 1; s <= 6; ++s) pts.push_back({a.x + (b.x - a.x) * s / 6.f, a.y + (b.y - a.y) * s / 6.f});
    }
    return pts;
}
