// The menu's text in other languages (i18n.h). Loaded once, at the first tr(): the language is the setting
// (menu_language) - Auto follows the game's language (settings_language) when a translation for it is shipped.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "i18n.h"

extern "C" {
#include "../paths.h"
#include "../settings.h"
}

static std::once_flag g_once;
static std::unordered_map<std::string, std::string> g_tr;
static std::string g_lang = "en", g_all;
static FILE *g_dump;
static std::unordered_set<std::string> g_dumped;
static std::mutex g_dump_mx;

static std::string unescape(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { o += '\n'; i++; }
        else if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 't') { o += '\t'; i++; }
        else if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\\') { o += '\\'; i++; }
        else o += s[i];
    }
    return o;
}

static bool load(const char *code) {
    char name[64], dev[128], path[1100];
    snprintf(name, sizeof name, "lang/%s.txt", code);
    snprintf(dev, sizeof dev, "runtime/ui/lang/%s.txt", code);
    if (!rt_resource(name, dev, path, sizeof path)) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    std::string line;
    char buf[8192];
    int n = 0;
    while (fgets(buf, sizeof buf, f)) {
        line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        // "English<TAB>translation" or "English ||| translation"
        size_t tab = line.find('\t'), sep = 1;
        if (tab == std::string::npos) tab = line.find(" ||| "), sep = 5;
        if (tab == std::string::npos) continue;
        std::string en = unescape(line.substr(0, tab)), to = unescape(line.substr(tab + sep));
        if (to.empty()) continue;
        g_all += to;
        g_tr.emplace(std::move(en), std::move(to));
        n++;
    }
    fclose(f);
    fprintf(stderr, "[i18n] menu in %s: %d texts (%s)\n", code, n, path);
    return n > 0;
}

static void init(void) {
    if (const char *d = getenv("ODYSSEY_I18N_DUMP")) g_dump = fopen(d, "ab");
    int m = g_settings.menu_language;
    // (Auto: the game's text language - for the Brazilian translation, which replaces en-US, Brazilian Portuguese)
    const char *want = m > 0 && m < N_LANGUAGES ? g_language_codes[m] : g_settings.language == LANG_PT_BR ? "pt-BR" : settings_language();
    if (want && strcmp(want, "en") && strncmp(want, "en-", 3) && load(want)) g_lang = want;
}

extern "C" const char *tr(const char *en) {
    if (!en || !*en) return en;
    std::call_once(g_once, init);
    if (g_dump) {
        std::lock_guard<std::mutex> l(g_dump_mx);
        if (g_dumped.insert(en).second) {
            std::string e;
            for (const char *p = en; *p; p++) e += *p == '\n' ? std::string("\\n") : *p == '\t' ? std::string("\\t") : std::string(1, *p);
            fprintf(g_dump, "%s\t\n", e.c_str());
            fflush(g_dump);
        }
    }
    if (g_tr.empty()) return en;
    auto it = g_tr.find(en);
    if (it != g_tr.end()) return it->second.c_str();
    // a text with a value in brackets made while running ("Desktop (2560 x 1440)", "Match display (2560 x 1440)"):
    // its words translated, the value kept (cached: the pointer stays valid)
    const char *br = strstr(en, " (");
    if (br && br > en) {
        auto p = g_tr.find(std::string(en, br));
        if (p != g_tr.end()) {
            static std::mutex mx;
            static std::unordered_map<std::string, std::string> made;
            std::lock_guard<std::mutex> l(mx);
            auto m = made.find(en);
            if (m == made.end()) m = made.emplace(en, p->second + br).first;
            return m->second.c_str();
        }
    }
    return en;
}

extern "C" const char *i18n_lang(void) {
    std::call_once(g_once, init);
    return g_lang.c_str();
}

extern "C" const char *i18n_all_text(void) {
    std::call_once(g_once, init);
    return g_tr.empty() ? NULL : g_all.c_str();
}
