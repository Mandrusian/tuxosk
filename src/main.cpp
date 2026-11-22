// tuxosk: on-screen keyboard for Wayland phones (wlroots/sway, phosh/phoc).
// input-method-v2 for text, virtual-keyboard-v1 for keys, wlr-layer-shell for the surface.
#include <glib-unix.h>
#include <gio/gio.h>
#include <glib.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <algorithm>
#include <fstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <map>
#include <csignal>
#include <glib/gstdio.h>
#include <algorithm>

#include "engine.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "ui.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

namespace {

bool gVerbose = false;
#define LOG(...) do { if (gVerbose) { std::fprintf(stderr, "[osk] " __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

int shmFile(size_t size) {
    int fd = memfd_create("tuxosk", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    return fd;
}

uint32_t nowMs() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

struct App;
App *gApp = nullptr;
GMainLoop *gLoop = nullptr;

struct Buf {
    wl_buffer *buf = nullptr;
    unsigned char *data = nullptr;
    bool busy = false;
    PxRect stale;
};

struct GTimers : Timers {
    std::set<int> live;
    std::function<void()> after;
    struct Ctx { GTimers *t; std::function<bool()> cb; int id; };
    int add(int ms, std::function<bool()> cb) override {
        auto *c = new Ctx{this, std::move(cb), 0};
        c->id = g_timeout_add(ms, [](gpointer p) -> gboolean {
            auto *c = (Ctx *)p;
            const bool again = c->cb();
            if (!again) c->t->live.erase(c->id);
            if (c->t->after) c->t->after();
            if (!again) { delete c; return G_SOURCE_REMOVE; }
            return G_SOURCE_CONTINUE;
        }, c);
        ctxs[c->id] = c;
        live.insert(c->id);
        return c->id;
    }
    void remove(int id) override {
        if (!id || !live.count(id)) return;
        live.erase(id);
        g_source_remove(id);
        auto it = ctxs.find(id);
        if (it != ctxs.end()) { delete it->second; ctxs.erase(it); }
    }
    std::map<int, Ctx *> ctxs;
};

struct App : Output {
    // wayland
    wl_display *dpy = nullptr;
    wl_registry *reg = nullptr;
    wl_compositor *comp = nullptr;
    wl_shm *shm = nullptr;
    wl_seat *seat = nullptr;
    wl_output *output = nullptr;
    zwlr_layer_shell_v1 *shell = nullptr;
    zwp_input_method_manager_v2 *imMgr = nullptr;
    zwp_virtual_keyboard_manager_v1 *vkMgr = nullptr;
    zwp_input_method_v2 *im = nullptr;
    zwp_virtual_keyboard_v1 *vk = nullptr;
    wl_touch *touch = nullptr;
    wl_pointer *pointer = nullptr;
    int outScale = 1, outW = 1080;
    bool mouseDown = false;

    // surface
    wl_surface *surf = nullptr;
    zwlr_layer_surface_v1 *ls = nullptr;
    wl_callback *frameCb = nullptr;
    Buf bufs[2];
    wl_shm_pool *pool = nullptr;
    int reqH = 0, curW = 0;
    bool configured = false;

    // full-screen dialog surface (language / settings), above everything
    wl_surface *msurf = nullptr;
    zwlr_layer_surface_v1 *mls = nullptr;
    bool mcfg = false;
    struct MBuf { wl_buffer *buf; wl_shm_pool *pool; void *map; size_t size; };
    void showModal() {
        if (msurf) return;
        msurf = wl_compositor_create_surface(comp);
        mls = zwlr_layer_shell_v1_get_layer_surface(shell, msurf, nullptr, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "tuxosk-dialog");
        static const zwlr_layer_surface_v1_listener l = {
            [](void *, zwlr_layer_surface_v1 *s, uint32_t serial, uint32_t w, uint32_t h) {
                App &a = *gApp;
                zwlr_layer_surface_v1_ack_configure(s, serial);
                a.ui->setModalSize((float)(w ? w : a.outW / a.outScale), (float)(h ? h : 1));
                a.mcfg = true;
                a.renderModal();
            },
            [](void *, zwlr_layer_surface_v1 *) { gApp->ui->closeModal(); }};
        zwlr_layer_surface_v1_add_listener(mls, &l, nullptr);
        zwlr_layer_surface_v1_set_anchor(mls, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                              ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_exclusive_zone(mls, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(mls, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        zwlr_layer_surface_v1_set_size(mls, 0, 0);
        wl_surface_set_buffer_scale(msurf, outScale);
        wl_surface_commit(msurf);
        wl_display_flush(dpy);
    }
    void hideModal() {
        if (!msurf) return;
        if (mls) { zwlr_layer_surface_v1_destroy(mls); mls = nullptr; }
        wl_surface_destroy(msurf); msurf = nullptr;
        mcfg = false;
        wl_display_flush(dpy);
    }
    void renderModal() {
        if (!msurf || !mcfg || !ui->takeModalDirty()) return;
        const int sc = outScale;
        const int pw = (int)(ui->modalW() * sc), ph = (int)(ui->modalH() * sc);
        if (pw <= 0 || ph <= 0) return;
        const int stride = pw * 4;
        MBuf *mb = new MBuf{};
        mb->size = (size_t)stride * ph;
        int fd = shmFile(mb->size);
        if (fd < 0) { delete mb; return; }
        mb->map = mmap(nullptr, mb->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        mb->pool = wl_shm_create_pool(shm, fd, (int)mb->size);
        close(fd);
        mb->buf = wl_shm_pool_create_buffer(mb->pool, 0, pw, ph, stride, WL_SHM_FORMAT_ARGB8888);
        static const wl_buffer_listener bl = {[](void *d, wl_buffer *b) {
            MBuf *m = (MBuf *)d;
            wl_buffer_destroy(b); wl_shm_pool_destroy(m->pool); munmap(m->map, m->size); delete m;
        }};
        wl_buffer_add_listener(mb->buf, &bl, mb);
        ui->blitModal((unsigned char *)mb->map, stride);
        wl_surface_attach(msurf, mb->buf, 0, 0);
        wl_surface_damage_buffer(msurf, 0, 0, pw, ph);
        wl_surface_commit(msurf);
        wl_display_flush(dpy);
    }

    // input method state
    uint32_t serial = 0;
    bool imOn = false, pendActive = false;
    uint32_t pendPurpose = 0, pendHint = 0, purpose = 0, hint = 0;
    std::string pendText; uint32_t pendCursor = 0; bool pendSurr = false;
    bool userHidden = false, dbusVisible = false, forceShow = false;
    std::string userDictPath, dictPath_, dictDir_, cfgPath_, filtersDir_;
    Settings cfg;
    bool ctlEnabled = false;

    // core
    Lexicon lex;
    SwipeDecoder dec;
    GTimers timers;
    Engine *eng = nullptr;
    Ui *ui = nullptr;

    // dbus
    GDBusConnection *bus = nullptr;

    // ---- Output
    bool imActive() const override { return imOn; }
    void commit(const std::string &s) override { LOG("commit '%s' im=%d on=%d serial=%u", s.c_str(), im != nullptr, (int)imOn, serial); if (im && imOn) zwp_input_method_v2_commit_string(im, s.c_str()); }
    void preedit(const std::string &s) override {
        if (im && imOn) zwp_input_method_v2_set_preedit_string(im, s.c_str(), (int)s.size(), (int)s.size());
    }
    void del(int before, int after) override { if (im && imOn) zwp_input_method_v2_delete_surrounding_text(im, before, after); }
    void key(uint32_t code, uint32_t mods) override {
        if (!vk) return;
        const uint32_t t = nowMs();
        uint32_t m = 0;
        if (mods & 1) m |= 1u << 0;
        if (mods & 2) m |= 1u << 2;
        if (mods & 4) m |= 1u << 3;
        if (m) zwp_virtual_keyboard_v1_modifiers(vk, m, 0, 0, 0);
        zwp_virtual_keyboard_v1_key(vk, t, code, WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(vk, t, code, WL_KEYBOARD_KEY_STATE_RELEASED);
        if (m) zwp_virtual_keyboard_v1_modifiers(vk, 0, 0, 0, 0);
        wl_display_flush(dpy);
    }
    void flush() override {
        if (im && imOn) zwp_input_method_v2_commit(im, serial);
        wl_display_flush(dpy);
    }

    bool wantVisible() const { return forceShow || dbusVisible || (imOn && !userHidden); }

    // ---- surface lifecycle
    void destroyBuffers() {
        for (auto &b : bufs) {
            if (b.buf) wl_buffer_destroy(b.buf);
            b = Buf{};
        }
        if (pool) { wl_shm_pool_destroy(pool); pool = nullptr; }
        if (mapped) { munmap(mapped, mappedSize); mapped = nullptr; }
    }
    unsigned char *mapped = nullptr;
    size_t mappedSize = 0;

    void makeBuffers() {
        destroyBuffers();
        const int w = ui->pxW(), h = ui->pxH(), stride = w * 4;
        mappedSize = (size_t)stride * h * 2;
        int fd = shmFile(mappedSize);
        if (fd < 0) { std::fprintf(stderr, "shm failed\n"); return; }
        mapped = (unsigned char *)mmap(nullptr, mappedSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        pool = wl_shm_create_pool(shm, fd, (int)mappedSize);
        close(fd);
        static const wl_buffer_listener bl = {[](void *d, wl_buffer *) { ((Buf *)d)->busy = false; if (gApp) gApp->render(); }};
        for (int i = 0; i < 2; ++i) {
            bufs[i].buf = wl_shm_pool_create_buffer(pool, (int32_t)(i * stride * h), w, h, stride, WL_SHM_FORMAT_ARGB8888);
            bufs[i].data = mapped + (size_t)i * stride * h;
            bufs[i].stale = {0, 0, w, h};
            wl_buffer_add_listener(bufs[i].buf, &bl, &bufs[i]);
        }
    }

    void show() {
        if (surf) return;
        LOG("show");
        curLayer = wantOverlay() ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY : ZWLR_LAYER_SHELL_V1_LAYER_TOP;
        surf = wl_compositor_create_surface(comp);
        ls = zwlr_layer_shell_v1_get_layer_surface(shell, surf, nullptr, curLayer, "tuxosk");
        static const zwlr_layer_surface_v1_listener l = {
            [](void *, zwlr_layer_surface_v1 *s, uint32_t serial, uint32_t w, uint32_t h) { gApp->configure(s, serial, (int)w, (int)h); },
            [](void *, zwlr_layer_surface_v1 *) { gApp->hide(); }};
        zwlr_layer_surface_v1_add_listener(ls, &l, nullptr);
        zwlr_layer_surface_v1_set_anchor(ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_keyboard_interactivity(ls, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        const int guess = (int)std::ceil(Layout::heightFor((float)outW / outScale));
        reqH = guess;
        zwlr_layer_surface_v1_set_size(ls, 0, reqH);
        zwlr_layer_surface_v1_set_exclusive_zone(ls, reqH);
        wl_surface_set_buffer_scale(surf, outScale);
        wl_surface_commit(surf);
        wl_display_flush(dpy);
        emitVisible();
    }

    void hide() {
        if (!surf) return;
        LOG("hide");
        if (frameCb) { wl_callback_destroy(frameCb); frameCb = nullptr; }
        ui->closeModalNow();
        if (ls) { zwlr_layer_surface_v1_destroy(ls); ls = nullptr; }
        wl_surface_destroy(surf); surf = nullptr;
        destroyBuffers();
        configured = false;
        curW = 0;
        ui->reset();
        wl_display_flush(dpy);
        emitVisible();
    }

    void configure(zwlr_layer_surface_v1 *s, uint32_t ser, int w, int) {
        zwlr_layer_surface_v1_ack_configure(s, ser);
        if (w <= 0) w = outW / outScale;
        const int need = (int)std::ceil(Layout::heightFor((float)w));
        if (w != curW || ui->scale() != outScale || !bufs[0].buf) {
            curW = w;
            ui->resize(w, outScale);
            makeBuffers();
        }
        if (need != reqH) {
            reqH = need;
            zwlr_layer_surface_v1_set_size(s, 0, need);
            zwlr_layer_surface_v1_set_exclusive_zone(s, need);
            wl_surface_commit(surf);
            return;
        }
        configured = true;
        ui->invalidateAll();
        render();
    }

    uint32_t curLayer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
    bool wantOverlay() const { return imOn && (purpose == 8 || purpose == 9); }   // lockscreen PIN/password sits above the TOP layer
    void sync() {
        if (surf && wantVisible() && curLayer != (wantOverlay() ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY : ZWLR_LAYER_SHELL_V1_LAYER_TOP)) hide();
        if (wantVisible()) show(); else hide();
    }

    // ---- render
    void render() {
        renderModal();
        if (!surf || !configured || frameCb || !ui->hasDirty()) return;
        Buf *b = nullptr, *o = nullptr;
        for (int i = 0; i < 2; ++i) if (!bufs[i].busy) { b = &bufs[i]; o = &bufs[1 - i]; break; }
        if (!b) return;
        const PxRect d = ui->takeDirty();
        PxRect r = d;
        r.add(b->stale);
        b->stale = {};
        r.x0 = std::max(0, r.x0); r.y0 = std::max(0, r.y0); r.x1 = std::min(ui->pxW(), r.x1); r.y1 = std::min(ui->pxH(), r.y1);
        if (r.empty()) return;
        ui->blit(b->data, ui->pxW() * 4, r);
        o->stale.add(d);
        wl_surface_attach(surf, b->buf, 0, 0);
        wl_surface_damage_buffer(surf, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
        static const wl_callback_listener cl = {[](void *, wl_callback *cb, uint32_t) {
            wl_callback_destroy(cb);
            if (gApp->frameCb == cb) gApp->frameCb = nullptr;
            gApp->render();
        }};
        frameCb = wl_surface_frame(surf);
        wl_callback_add_listener(frameCb, &cl, nullptr);
        b->busy = true;
        wl_surface_commit(surf);
        wl_display_flush(dpy);
    }

    // ---- input method
    void imDone() {
        const bool was = imOn;
        const uint32_t oldPurpose = purpose, oldHint = hint;
        imOn = pendActive;
        purpose = pendPurpose; hint = pendHint;
        ++serial;
        if (imOn && !was) {
            userHidden = false;
            eng->activated();
        }
        if (!imOn && was) { eng->deactivated(); userHidden = false; saveDict(); }
        if (imOn) {
            Context c;
            c.numeric = purpose == 2 || purpose == 3 || purpose == 4 || purpose == 9;
            c.email = purpose == 6;
            c.url = purpose == 5;
            c.terminal = purpose == 13;
            const bool nop = purpose == 8 || purpose == 13 || (hint & 0x40) || (hint & 0x80) || c.numeric;
            if (!was || oldPurpose != purpose || oldHint != hint) {
                ui->setContext(c);
                eng->setNoPredict(nop);
            }
            if (pendSurr) eng->surrounding(pendText, pendCursor);
        }
        pendSurr = false;
        LOG("done active=%d purpose=%u hint=%x", imOn, purpose, hint);
        sync();
        render();
    }

    // ---- settings
    static std::string langLabel(const std::string &c) {
        static const std::map<std::string, std::string> m = {{"en", "English (US)"}, {"de", "Deutsch"}, {"fr", "Français"}, {"es", "Español"},
            {"it", "Italiano"}, {"pt", "Português"}, {"nl", "Nederlands"}, {"pl", "Polski"}, {"sv", "Svenska"}, {"ru", "Русский"}};
        auto it = m.find(c);
        return it == m.end() ? c : it->second;
    }
    void scanLists() {
        std::vector<Ui::Lang> langs;
        std::vector<Ui::Filter> filters;
        if (GDir *d = g_dir_open(dictDir_.c_str(), 0, nullptr)) {
            while (const char *n = g_dir_read_name(d)) {
                const std::string f = n;
                if (f.size() >= 5 && f.size() <= 7 && f.compare(f.size() - 4, 4, ".txt") == 0) {
                    const std::string c = f.substr(0, f.size() - 4);
                    bool ok = true;
                    for (char ch : c) if (ch < 'a' || ch > 'z') ok = false;
                    if (ok && c != "user") langs.push_back({c, langLabel(c), "QWERTY"});
                }
            }
            g_dir_close(d);
        }
        std::sort(langs.begin(), langs.end(), [](const Ui::Lang &a, const Ui::Lang &b) { return a.code < b.code; });
        if (langs.empty()) langs.push_back({"en", "English (US)", "QWERTY"});
        if (access((dictDir_ + "/blocklist.txt").c_str(), R_OK) == 0) filters.push_back({"slurs", "Slurs"});
        if (GDir *d = g_dir_open(filtersDir_.c_str(), 0, nullptr)) {
            while (const char *n = g_dir_read_name(d)) {
                const std::string f = n;
                if (f.size() > 4 && f.compare(f.size() - 4, 4, ".txt") == 0) {
                    std::string id = f.substr(0, f.size() - 4), label = id == "mywords" ? "My words" : id;
                    if (!label.empty()) label[0] = (char)std::toupper((unsigned char)label[0]);
                    filters.push_back({id, label});
                    if (!cfg.filters.count(id)) cfg.filters[id] = true;
                }
            }
            g_dir_close(d);
        }
        ui->setLanguages(langs);
        ui->setFilters(filters);
    }
    void reloadDict() {
        saveDict();
        lex = Lexicon();
        for (auto &[n, on] : cfg.filters)
            if (on) lex.loadBlocklist(n == "slurs" ? dictDir_ + "/blocklist.txt" : filtersDir_ + "/" + n + ".txt");
        std::string path = dictDir_ + "/" + cfg.lang + ".txt";
        if (access(path.c_str(), R_OK) != 0) path = dictPath_;
        lex.load(path);
        lex.loadUser(userDictPath);
        dec.setLexicon(&lex);
    }
    void relayout() {
        if (!surf || !ls) return;
        const int need = (int)std::ceil(Layout::heightFor((float)std::max(curW, 1)));
        reqH = need;
        zwlr_layer_surface_v1_set_size(ls, 0, need);
        zwlr_layer_surface_v1_set_exclusive_zone(ls, need);
        wl_surface_commit(surf);
        curW = 0; configured = false;
    }
    void applyLook() {
        Layout::heightScale = cfg.tall ? 1.12f : 1.0f;
        ui->setTheme(cfg.light ? Ui::lightTheme() : Theme());
        eng->setAutocorrect(cfg.autocorrect);
        eng->setAutocap(cfg.autocap);
    }
    void settingChanged(const std::string &id) {
        cfg.save(cfgPath_);
        if (id == "language" || id == "filters") { reloadDict(); scanLists(); }
        else if (id == "tall_keys") { applyLook(); relayout(); }
        else applyLook();
        render();
    }
    void blockWord(const std::string &w) {
        g_mkdir_with_parents(filtersDir_.c_str(), 0755);
        std::ofstream o(filtersDir_ + "/mywords.txt", std::ios::app);
        o << w << "\n";
        o.close();
        cfg.filters["mywords"] = true;
        cfg.save(cfgPath_);
        reloadDict(); scanLists();
        render();
    }

    void saveDict() { if (!userDictPath.empty()) lex.saveUser(userDictPath); }

    // ---- dbus
    void emitVisible() {
        if (!bus) return;
        GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "Visible", g_variant_new_boolean(surf != nullptr));
        g_dbus_connection_emit_signal(bus, nullptr, "/sm/puri/OSK0", "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                      g_variant_new("(sa{sv}as)", "sm.puri.OSK0", &b, nullptr), nullptr);
    }
    void setVisible(bool v) {
        LOG("dbus SetVisible %d", v);
        if (imOn) { userHidden = !v; dbusVisible = false; }
        else dbusVisible = v;
        sync();
    }
};

static std::set<int> &modalIds() { static std::set<int> s; return s; }

// ---- registry / seat / im callbacks (free functions)

void regGlobal(void *, wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    App &a = *gApp;
    if (!std::strcmp(iface, "wl_compositor")) a.comp = (wl_compositor *)wl_registry_bind(r, name, &wl_compositor_interface, std::min(ver, 4u));
    else if (!std::strcmp(iface, "wl_shm")) a.shm = (wl_shm *)wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!std::strcmp(iface, "wl_seat") && !a.seat) a.seat = (wl_seat *)wl_registry_bind(r, name, &wl_seat_interface, std::min(ver, 5u));
    else if (!std::strcmp(iface, "zwlr_layer_shell_v1")) a.shell = (zwlr_layer_shell_v1 *)wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, std::min(ver, 4u));
    else if (!std::strcmp(iface, "zwp_input_method_manager_v2")) a.imMgr = (zwp_input_method_manager_v2 *)wl_registry_bind(r, name, &zwp_input_method_manager_v2_interface, 1);
    else if (!std::strcmp(iface, "zwp_virtual_keyboard_manager_v1")) a.vkMgr = (zwp_virtual_keyboard_manager_v1 *)wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    else if (!std::strcmp(iface, "wl_output") && !a.output) {
        a.output = (wl_output *)wl_registry_bind(r, name, &wl_output_interface, std::min(ver, 2u));
        wl_output_listener ol{};
        ol.geometry = [](void *, wl_output *, int, int, int, int, int, const char *, const char *, int) {};
        ol.mode = [](void *, wl_output *, uint32_t flags, int w, int, int) { if (flags & WL_OUTPUT_MODE_CURRENT) gApp->outW = w; };
        ol.done = [](void *, wl_output *) {};
        ol.scale = [](void *, wl_output *, int s) {
            gApp->outScale = std::max(1, s);
            if (gApp->surf && gApp->ui->scale() != gApp->outScale) {
                wl_surface_set_buffer_scale(gApp->surf, gApp->outScale);
                if (gApp->configured) gApp->configure(gApp->ls, 0, gApp->curW, 0);
            }
        };
        static wl_output_listener keep; keep = ol;
        wl_output_add_listener(a.output, &keep, nullptr);
    }
}
void regRemove(void *, wl_registry *, uint32_t) {}

void setupKeymap(App &a) {
    xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_rule_names names{};
    names.layout = "us";
    xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    char *str = km ? xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1) : nullptr;
    if (!str) { std::fprintf(stderr, "xkb keymap failed\n"); return; }
    const size_t n = std::strlen(str) + 1;
    int fd = shmFile(n);
    void *m = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    std::memcpy(m, str, n);
    munmap(m, n);
    zwp_virtual_keyboard_v1_keymap(a.vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)n);
    close(fd);
    free(str);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
}

// ---- wayland fd as GSource
struct WlSource { GSource base; App *app; GPollFD pfd; };

gboolean wlPrepare(GSource *s, gint *timeout) {
    App *a = ((WlSource *)s)->app;
    *timeout = -1;
    while (wl_display_prepare_read(a->dpy) != 0) wl_display_dispatch_pending(a->dpy);
    wl_display_flush(a->dpy);
    return FALSE;
}
gboolean wlCheck(GSource *s) {
    WlSource *w = (WlSource *)s;
    if (w->pfd.revents & (G_IO_IN | G_IO_ERR | G_IO_HUP)) { wl_display_read_events(w->app->dpy); return TRUE; }
    wl_display_cancel_read(w->app->dpy);
    return FALSE;
}
gboolean wlDispatch(GSource *s, GSourceFunc, gpointer) {
    App *a = ((WlSource *)s)->app;
    if (wl_display_dispatch_pending(a->dpy) < 0) { g_main_loop_quit(gLoop); }
    a->render();
    return TRUE;
}

void busMethod(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *method, GVariant *params,
               GDBusMethodInvocation *inv, gpointer) {
    if (!std::strcmp(method, "SetVisible")) {
        gboolean v = FALSE;
        g_variant_get(params, "(b)", &v);
        gApp->setVisible(v);
        g_dbus_method_invocation_return_value(inv, nullptr);
    } else g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown");
}
GVariant *busProp(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *prop, GError **, gpointer) {
    if (!std::strcmp(prop, "Visible")) return g_variant_new_boolean(gApp->surf != nullptr);
    return nullptr;
}

void connectIm() {
    if (!gApp->imMgr || gApp->im) return;
        gApp->im = zwp_input_method_manager_v2_get_input_method(gApp->imMgr, gApp->seat);
        static zwp_input_method_v2_listener il{};
        il.activate = [](void *, zwp_input_method_v2 *) { gApp->pendActive = true; gApp->pendPurpose = 0; gApp->pendHint = 0; gApp->pendSurr = false; };
        il.deactivate = [](void *, zwp_input_method_v2 *) { gApp->pendActive = false; };
        il.surrounding_text = [](void *, zwp_input_method_v2 *, const char *t, uint32_t cur, uint32_t) {
            gApp->pendText = t; gApp->pendCursor = cur; gApp->pendSurr = true;
        };
        il.text_change_cause = [](void *, zwp_input_method_v2 *, uint32_t) {};
        il.content_type = [](void *, zwp_input_method_v2 *, uint32_t hint, uint32_t purpose) { gApp->pendHint = hint; gApp->pendPurpose = purpose; };
        il.done = [](void *, zwp_input_method_v2 *) { gApp->imDone(); };
        il.unavailable = [](void *, zwp_input_method_v2 *) {
            std::fprintf(stderr, "input method unavailable (another IME running?); keys only\n");
            gApp->im = nullptr; g_timeout_add(1000, [](gpointer) -> gboolean { connectIm(); return G_SOURCE_REMOVE; }, nullptr);
        };
        zwp_input_method_v2_add_listener(gApp->im, &il, nullptr);
    }

}  // namespace

int main(int argc, char **argv) {
    App app;
    gApp = &app;
    std::string dictPath;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-v")) gVerbose = true;
        else if (!std::strcmp(argv[i], "--show")) app.forceShow = true;
        else if (!std::strcmp(argv[i], "--ctl")) app.ctlEnabled = true;
        else if (!std::strcmp(argv[i], "--dict") && i + 1 < argc) dictPath = argv[++i];
        else { std::fprintf(stderr, "usage: tuxosk [-v] [--show] [--dict file] [--ctl]\n"); return 2; }
    }
    if (dictPath.empty()) {
        std::vector<std::string> c;
        if (const char *e = std::getenv("OSK_DICT")) c.push_back(e);
        if (const char *h = std::getenv("HOME")) c.push_back(std::string(h) + "/.local/share/tuxosk/en.txt");
        c.push_back("data/en.txt");
        c.push_back("/usr/local/share/tuxosk/en.txt");
        c.push_back("/usr/share/tuxosk/en.txt");
        for (auto &p : c) if (access(p.c_str(), R_OK) == 0) { dictPath = p; break; }
    }
    if (!dictPath.empty()) {
        const size_t sl = dictPath.find_last_of('/');
        app.dictDir_ = sl == std::string::npos ? "." : dictPath.substr(0, sl);
        app.dictPath_ = dictPath;
    }
    {
        const char *home = std::getenv("HOME");
        const char *xc = std::getenv("XDG_CONFIG_HOME");
        std::string cdir = (xc ? std::string(xc) : std::string(home ? home : ".") + "/.config") + "/tuxosk";
        g_mkdir_with_parents(cdir.c_str(), 0755);
        app.cfgPath_ = cdir + "/config";
        app.filtersDir_ = cdir + "/filters";
        app.cfg.load(app.cfgPath_);
        if (access(app.cfgPath_.c_str(), R_OK) != 0) app.cfg.save(app.cfgPath_);
    }
    for (auto &[n, on] : app.cfg.filters)
        if (on) app.lex.loadBlocklist(n == "slurs" ? app.dictDir_ + "/blocklist.txt" : app.filtersDir_ + "/" + n + ".txt");
    {
        std::string lp = app.dictDir_ + "/" + app.cfg.lang + ".txt";
        if (access(lp.c_str(), R_OK) == 0) dictPath = lp;
    }
    if (dictPath.empty() || !app.lex.load(dictPath)) { std::fprintf(stderr, "dictionary not found\n"); return 1; }
    {
        const char *home = std::getenv("HOME");
        const char *xdg = std::getenv("XDG_DATA_HOME");
        std::string dir = xdg ? std::string(xdg) : std::string(home ? home : ".") + "/.local/share";
        dir += "/tuxosk";
        g_mkdir_with_parents(dir.c_str(), 0755);
        app.userDictPath = dir + "/user.txt";
        app.lex.loadUser(app.userDictPath);
    }
    app.dec.setLexicon(&app.lex);

    app.dpy = wl_display_connect(nullptr);
    if (!app.dpy) { std::fprintf(stderr, "no wayland display\n"); return 1; }
    app.reg = wl_display_get_registry(app.dpy);
    wl_registry_listener rl{regGlobal, regRemove};
    wl_registry_add_listener(app.reg, &rl, nullptr);
    wl_display_roundtrip(app.dpy);
    wl_display_roundtrip(app.dpy);
    if (!app.comp || !app.shm || !app.seat || !app.shell) { std::fprintf(stderr, "missing wl_compositor/wl_shm/wl_seat/layer-shell\n"); return 1; }
    if (!app.imMgr) std::fprintf(stderr, "warning: no input-method-v2: keys only\n");
    if (!app.vkMgr) std::fprintf(stderr, "warning: no virtual-keyboard-v1\n");

    app.eng = new Engine(app.lex, app.dec, app);
    app.ui = new Ui(*app.eng, app.lex, app.dec, app.timers);
    app.ui->setSettings(&app.cfg);
    app.ui->onSettingChanged = [](const std::string &id) { gApp->settingChanged(id); };
    app.ui->onBlockWord = [](const std::string &w) { gApp->blockWord(w); };
    app.ui->onHide = [] {
        App &a = *gApp;
        a.forceShow = false;
        a.setVisible(false);
    };
    app.ui->onModalVisible = [](bool v) { if (v) gApp->showModal(); else gApp->hideModal(); };
    app.scanLists();
    app.applyLook();
    app.timers.after = [] { gApp->render(); };

    if (app.vkMgr) {
        app.vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(app.vkMgr, app.seat);
        setupKeymap(app);
    }
    // touch + pointer (mouse emulates a finger for testing)
    static wl_seat_listener sl{};
    sl.capabilities = [](void *, wl_seat *s, uint32_t caps) {
        App &a = *gApp;
        if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !a.touch) {
            a.touch = wl_seat_get_touch(s);
            static wl_touch_listener tl{};
            tl.down = [](void *, wl_touch *, uint32_t, uint32_t, wl_surface *sf, int32_t id, wl_fixed_t x, wl_fixed_t y) {
                if (sf == gApp->msurf && sf) { modalIds().insert(id); gApp->ui->modalTouchDown((float)wl_fixed_to_double(x), (float)wl_fixed_to_double(y)); gApp->render(); return; }
                if (sf != gApp->surf) return;
                gApp->ui->touchDown(id, (float)wl_fixed_to_double(x), (float)wl_fixed_to_double(y)); gApp->render();
            };
            tl.up = [](void *, wl_touch *, uint32_t, uint32_t, int32_t id) {
                if (modalIds().erase(id)) { gApp->ui->modalTouchUp(); gApp->render(); return; }
                gApp->ui->touchUp(id); gApp->render(); };
            tl.motion = [](void *, wl_touch *, uint32_t, int32_t id, wl_fixed_t x, wl_fixed_t y) {
                if (modalIds().count(id)) return;
                gApp->ui->touchMotion(id, (float)wl_fixed_to_double(x), (float)wl_fixed_to_double(y)); gApp->render();
            };
            tl.frame = [](void *, wl_touch *) {};
            tl.cancel = [](void *, wl_touch *) { modalIds().clear(); for (int i = 0; i < 10; ++i) gApp->ui->touchCancel(i); gApp->render(); };
            tl.shape = [](void *, wl_touch *, int32_t, wl_fixed_t, wl_fixed_t) {};
            tl.orientation = [](void *, wl_touch *, int32_t, wl_fixed_t) {};
            wl_touch_add_listener(a.touch, &tl, nullptr);
        }
        if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a.pointer) {
            a.pointer = wl_seat_get_pointer(s);
            static wl_pointer_listener pl{};
            static float px = 0, py = 0; static bool over = false, overM = false;
            pl.enter = [](void *, wl_pointer *, uint32_t, wl_surface *sf, wl_fixed_t x, wl_fixed_t y) {
                over = sf == gApp->surf; overM = sf && sf == gApp->msurf; px = (float)wl_fixed_to_double(x); py = (float)wl_fixed_to_double(y);
            };
            pl.leave = [](void *, wl_pointer *, uint32_t, wl_surface *) {
                over = false; overM = false; if (gApp->mouseDown) { gApp->mouseDown = false; gApp->ui->touchCancel(99); gApp->render(); }
            };
            pl.motion = [](void *, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y) {
                px = (float)wl_fixed_to_double(x); py = (float)wl_fixed_to_double(y);
                if (gApp->mouseDown) { gApp->ui->touchMotion(99, px, py); gApp->render(); }
            };
            pl.button = [](void *, wl_pointer *, uint32_t, uint32_t, uint32_t btn, uint32_t st) {
                if (overM && btn == 0x110) {
                    if (st == WL_POINTER_BUTTON_STATE_PRESSED) gApp->ui->modalTouchDown(px, py); else gApp->ui->modalTouchUp();
                    gApp->render(); return;
                }
                if (!over || btn != 0x110) return;
                if (st == WL_POINTER_BUTTON_STATE_PRESSED) { gApp->mouseDown = true; gApp->ui->touchDown(99, px, py); }
                else if (gApp->mouseDown) { gApp->mouseDown = false; gApp->ui->touchUp(99); }
                gApp->render();
            };
            pl.axis = [](void *, wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {};
            pl.frame = [](void *, wl_pointer *) {};
            pl.axis_source = [](void *, wl_pointer *, uint32_t) {};
            pl.axis_stop = [](void *, wl_pointer *, uint32_t, uint32_t) {};
            pl.axis_discrete = [](void *, wl_pointer *, uint32_t, int32_t) {};
            wl_pointer_add_listener(a.pointer, &pl, nullptr);
        }
    };
    sl.name = [](void *, wl_seat *, const char *) {};
    wl_seat_add_listener(app.seat, &sl, nullptr);
    wl_display_roundtrip(app.dpy);

    // test control: $XDG_RUNTIME_DIR/tuxosk.fifo  (tap <label> | swipe <word> | hold <label> | state)
    if (const char *rt = app.ctlEnabled ? std::getenv("XDG_RUNTIME_DIR") : nullptr) {
        static std::string path = std::string(rt) + "/tuxosk.fifo";
        mkfifo(path.c_str(), 0600);
        const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd >= 0) g_io_add_watch(g_io_channel_unix_new(fd), G_IO_IN, [](GIOChannel *ch, GIOCondition, gpointer) -> gboolean {
            static std::string buf;
            char tmp[256];
            const ssize_t n = read(g_io_channel_unix_get_fd(ch), tmp, sizeof tmp);
            if (n <= 0) return TRUE;
            buf.append(tmp, n);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl); buf.erase(0, nl + 1);
                const size_t sp = line.find(' ');
                const std::string cmd = line.substr(0, sp), arg = sp == std::string::npos ? "" : line.substr(sp + 1);
                Ui &u = *gApp->ui;
                float x, y;
                if (cmd == "tap" && u.keyPos(arg, x, y)) { u.touchDown(50, x, y); u.touchUp(50); }
                else if (cmd == "down" && u.keyPos(arg, x, y)) u.touchDown(50, x, y);
                else if (cmd == "up") u.touchUp(50);
                else if (cmd == "xy") { float px = 0, py = 0; if (std::sscanf(arg.c_str(), "%f %f", &px, &py) == 2) { if (u.modalOpen()) { u.modalTouchDown(px, py); u.modalTouchUp(); } else { u.touchDown(50, px, py); u.touchUp(50); } } }
                else if (cmd == "swipe") {
                    auto p = u.wordPath(arg);
                    if (!p.empty()) { u.touchDown(50, p[0].x, p[0].y); for (size_t i = 1; i < p.size(); ++i) u.touchMotion(50, p[i].x, p[i].y); u.touchUp(50); }
                } else if (cmd == "hold" && u.keyPos(arg, x, y)) {
                    u.touchDown(50, x, y);
                    g_timeout_add(900, [](gpointer) -> gboolean { gApp->ui->touchUp(50); gApp->render(); return G_SOURCE_REMOVE; }, nullptr);
                }
                LOG("ctl %s", line.c_str());
                gApp->render();
            }
            return TRUE;
        }, nullptr);
    }

    // dbus: phosh drives the keyboard through sm.puri.OSK0
    static const char *xml =
        "<node><interface name='sm.puri.OSK0'><method name='SetVisible'><arg type='b' name='visible' direction='in'/></method>"
        "<property name='Visible' type='b' access='read'/></interface></node>";
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(xml, nullptr);
    static const GDBusInterfaceVTable vt = {busMethod, busProp, nullptr, {}};
    g_bus_own_name(G_BUS_TYPE_SESSION, "sm.puri.OSK0", G_BUS_NAME_OWNER_FLAGS_REPLACE,
                   [](GDBusConnection *c, const gchar *, gpointer p) {
                       gApp->bus = c;
                       g_dbus_connection_register_object(c, "/sm/puri/OSK0", ((GDBusNodeInfo *)p)->interfaces[0], &vt, nullptr, nullptr, nullptr);
                   },
                   [](GDBusConnection *, const gchar *, gpointer) { LOG("dbus name acquired"); connectIm(); },
                   [](GDBusConnection *, const gchar *, gpointer) { LOG("dbus name lost"); }, info, nullptr);

    gLoop = g_main_loop_new(nullptr, FALSE);
    static GSourceFuncs funcs = {wlPrepare, wlCheck, wlDispatch, nullptr, nullptr, nullptr};
    WlSource *src = (WlSource *)g_source_new(&funcs, sizeof(WlSource));
    src->app = &app;
    src->pfd.fd = wl_display_get_fd(app.dpy);
    src->pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
    g_source_add_poll(&src->base, &src->pfd);
    g_source_attach(&src->base, nullptr);
    g_unix_signal_add(SIGTERM, [](gpointer) -> gboolean { g_main_loop_quit(gLoop); return G_SOURCE_REMOVE; }, nullptr);
    g_unix_signal_add(SIGINT, [](gpointer) -> gboolean { g_main_loop_quit(gLoop); return G_SOURCE_REMOVE; }, nullptr);
    g_timeout_add_seconds(120, [](gpointer) -> gboolean { gApp->saveDict(); return G_SOURCE_CONTINUE; }, nullptr);

    g_timeout_add(2500, [](gpointer) -> gboolean { connectIm(); return G_SOURCE_REMOVE; }, nullptr);
    app.sync();
    LOG("running, dict %s, %zu words", dictPath.c_str(), app.lex.words().size());
    g_main_loop_run(gLoop);
    app.saveDict();
    app.hide();
    return 0;
}
