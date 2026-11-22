// User settings, stored as key=value lines in $XDG_CONFIG_HOME/tiles-osk/config.
#pragma once
#include <fstream>
#include <map>
#include <string>

struct Settings {
    // word filters: name -> enabled. "slurs" ships with the keyboard; others are lists in ~/.config/tiles-osk/filters/<name>.txt
    std::map<std::string, bool> filters{{"slurs", true}};
    bool autocorrect = true;
    bool autocap = true;
    bool preview = true;         // key preview bubble
    bool light = false;          // light theme
    bool tall = false;           // taller keys
    bool reduceMotion = false;   // no animations
    std::string lang = "en";

    void load(const std::string &path) {
        std::ifstream in(path);
        std::string l;
        auto on = [](const std::string &v) { return v.rfind("off", 0) != 0 && v.rfind("0", 0) != 0 && v.rfind("false", 0) != 0; };
        while (std::getline(in, l)) {
            const size_t eq = l.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
            if (k.rfind("filter.", 0) == 0) filters[k.substr(7)] = on(v);
            else if (k == "autocorrect") autocorrect = on(v);
            else if (k == "autocap") autocap = on(v);
            else if (k == "key_preview") preview = on(v);
            else if (k == "light_theme") light = on(v);
            else if (k == "tall_keys") tall = on(v);
            else if (k == "reduce_motion") reduceMotion = on(v);
            else if (k == "language" && !v.empty()) lang = v;
        }
    }
    void save(const std::string &path) const {
        std::ofstream o(path, std::ios::trunc);
        auto b = [](bool v) { return v ? "on" : "off"; };
        o << "language=" << lang << "\n";
        for (auto &[n, e] : filters) o << "filter." << n << "=" << b(e) << "\n";
        o << "autocorrect=" << b(autocorrect)
          << "\nautocap=" << b(autocap) << "\nkey_preview=" << b(preview) << "\nlight_theme=" << b(light)
          << "\ntall_keys=" << b(tall) << "\nreduce_motion=" << b(reduceMotion) << "\n";
    }
};
