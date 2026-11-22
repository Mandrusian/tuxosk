// Renders the keyboard to PNGs without Wayland: tests/snapshot data/en.txt /tmp/out
#include <cstdio>
#include "../src/ui.h"

struct Null : Output {
    std::string t;
    void commit(const std::string &s) override { t += s; }
    void preedit(const std::string &) override {}
    void del(int, int) override {}
    void key(uint32_t, uint32_t) override {}
    void flush() override {}
    bool imActive() const override { return true; }
};
struct NoTimers : Timers { int add(int, std::function<bool()>) override { return 0; } void remove(int) override {} };

int main(int argc, char **argv) {
    Lexicon lex; if (!lex.load(argv[1])) return 2;
    SwipeDecoder dec; dec.setLexicon(&lex);
    Null out; Engine eng(lex, dec, out); NoTimers tm;
    Ui ui(eng, lex, dec, tm);
    ui.resize(540, 2);
    eng.activated();
    std::string dir = argv[2];
    ui.savePng((dir + "_letters.png").c_str());
    // typing "hel" shows suggestions; finger down on 'e' shows the preview
    for (char c : std::string("hel")) eng.typeChar(std::string(1, c));
    ui.touchDown(1, 180, 150);
    ui.savePng((dir + "_typing.png").c_str());
    ui.touchUp(1);
    // swipe h-e-l-l-o trail
    ui.touchDown(2, 340, 150);
    for (int i = 0; i <= 20; ++i) ui.touchMotion(2, 340 - i * 6, 150 + (i < 10 ? -i * 4 : i * 2));
    ui.savePng((dir + "_swipe.png").c_str());
    ui.touchUp(2);
    { Context c; c.terminal = true; ui.setContext(c); ui.touchDown(3, 150, 20); ui.touchUp(3); ui.savePng((dir + "_term.png").c_str()); }
    {
        Settings st; ui.setSettings(&st);
        ui.setLanguages({{"en", "English (US)", "QWERTY"}});
        ui.setFilters({{"slurs", "Slurs"}, {"mywords", "My words"}});
        st.filters["mywords"] = false;
        { Context c; ui.setContext(c); }
        ui.openLangs(); ui.savePng((dir + "_langs.png").c_str());
        ui.openSettings(); ui.savePng((dir + "_settings.png").c_str());
        ui.openFilters(); ui.savePng((dir + "_filters.png").c_str());
        ui.closeModal(); ui.setTheme(Ui::lightTheme()); st.light = true; ui.openSettings(); ui.savePng((dir + "_light.png").c_str());
    }
    std::printf("ok h=%d\n", ui.pxH());
}
