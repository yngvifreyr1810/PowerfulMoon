// The menu's Language (Settings > System language) over the save's. Odyssey keeps its text language in the save's
// Common.bin ("USen", "EUde", ...: the region and language of its LocalizedData folders) and reads it back at every
// start; the system language (runtime/services/am.c GetDesiredLanguage) only picks one for a new save. So with an
// existing save the menu's choice did nothing (#17). Auto keeps the save's language (the game's own Options >
// Language changes it); any other choice replaces it, and the game then saves that one.
//
// GameDataHolder::readFromSaveDataBufferCommonFileOnlyLanguage (Application::init, a direct call) is replaced at
// generation (runtime/hle/hle_symbols.txt; 1.4.1 by address: tools/odyssey_patches.py HLE_141) by the body below:
// what the game's does - the save buffer's first 0x38 bytes, a version word then the language at +0xC, taken when the
// version is one it knows (1.0.0: 0; 1.4.1: 0 or 3) - then the menu's choice.
#include <string.h>

#include "../hle.h"
#include "../settings.h"

static uint64_t code_mem;

// The game's code for the menu's language (settings.c g_language_codes): the region it ships that language in
static const char *game_code(const char *lang) {
    static const struct { const char *lang, *code; } map[] = {
        {"ja", "JPja"}, {"en-US", "USen"}, {"es", "EUes"}, {"fr", "EUfr"}, {"de", "EUde"}, {"it", "EUit"},
        {"nl", "EUnl"}, {"ru", "EUru"}, {"ko", "KRko"}, {"zh-CN", "CNzh"}, {"zh-TW", "TWzh"},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(lang, map[i].lang)) return map[i].code;
    return NULL;
}

static void set_language(Cpu *c, uint64_t holder, uint64_t str) {
    static uint64_t fn;
    if (!fn) fn = guest_sym("_ZN14GameDataHolder11setLanguageEPKc");
    c->x[0] = holder;
    c->x[1] = str;
    guest_call(c, fn);
}

void hle__ZN14GameDataHolder44readFromSaveDataBufferCommonFileOnlyLanguageEv(Cpu *c) {
    uint64_t holder = c->x[0];
    Cpu saved = *c;
    c->sp = (c->sp - 0x100) & ~0xFull;
    guest_call(c, guest_sym("_ZN2al21getSaveDataWorkBufferEv"));
    uint64_t buf = c->x[0];
    if (buf >= GUEST_LO && buf < GUEST_HI - 0x38) {
        uint32_t version;
        memcpy(&version, (const void *)(uintptr_t)buf, 4);
        if (version == 0 || (version == 3 && loader_main_is_v141())) {  // (the game's own check)
            char lang[0x38 - 0xC + 1] = {0};
            memcpy(lang, (const void *)(uintptr_t)(buf + 0xC), sizeof lang - 1);
            if (!code_mem) code_mem = (uint64_t)(uintptr_t)hle_alloc(0x40);
            memcpy((void *)(uintptr_t)code_mem, lang, sizeof lang);  // (a copy: its "%s" reads a terminated string)
            set_language(c, holder, code_mem);
        }
    }
    const char *code = g_settings.language > LANG_AUTO && g_settings.language < N_LANGUAGES
                       ? game_code(g_language_codes[g_settings.language]) : NULL;
    if (code) {
        if (!code_mem) code_mem = (uint64_t)(uintptr_t)hle_alloc(0x40);
        memcpy((void *)(uintptr_t)code_mem, code, strlen(code) + 1);
        set_language(c, holder, code_mem);
        LOG("[language] %s from the menu, over the save's\n", code);
    }
    *c = saved;
}
