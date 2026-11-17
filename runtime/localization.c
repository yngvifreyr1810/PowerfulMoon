#include "localization.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mods.h"
#include "nxcrypto.h"
#include "utf8file.h"

static const struct { const char *path, *sha; size_t size; } ptbr_files[PTBR_FILES] = {
    {"LocalizedData/USen/LayoutData/FontData.szs",
     "b5f0c5eabdba5dbb6d9432d8c124cfe30733968580ab2267423cd87f91911ae7", 579765},
    {"LocalizedData/USen/MessageData/LayoutMessage.szs",
     "1a661766c2e4cb22552e1da57b37732a7f6988e31a00745e26c4704bbb37320e", 14583},
    {"LocalizedData/USen/MessageData/StageMessage.szs",
     "694c588babce792625182398d2fc303bb056f2d229ef677c891f08ec5f3f1046", 190394},
    {"LocalizedData/USen/MessageData/SystemMessage.szs",
     "3660c091415a65067561fa9364d8b4d5290f1d62a19bcdadc1049373a0556450", 279530},
};

int localization_ptbr_file(const char *rel) {
    for (int i = 0; i < PTBR_FILES; i++)
        if (!strcasecmp(rel, ptbr_files[i].path)) return i;
    return -1;
}

const char *localization_ptbr_path(int file) {
    return file >= 0 && file < PTBR_FILES ? ptbr_files[file].path : NULL;
}

bool localization_ptbr_matches(int file, const char *host) {
    if (file < 0 || file >= PTBR_FILES) return false;
    FILE *f = rt_fopen_utf8(host, "rb");
    if (!f) return false;
    size_t size = ptbr_files[file].size;
    // Limita a leitura mesmo se um pacote desconhecido contiver arquivos muito grandes.
    uint8_t *data = malloc(size);
    bool ok = data && fread(data, 1, size, f) == size && fgetc(f) == EOF && !ferror(f);
    fclose(f);
    if (ok) {
        uint8_t hash[32];
        char hex[65];
        nx_sha256(data, size, hash);
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
        ok = !strcmp(hex, ptbr_files[file].sha);
    }
    free(data);
    return ok;
}

int localization_ptbr_status(void) {
    unsigned paths = 0, matches = 0;
    bool known = false, enabled = false, complete = false, code = false;
    // A mesma prioridade de mods_files: o primeiro mod habilitado vence por caminho.
    for (int i = 0; i < mods_count(); i++) {
        const ModInfo *m = mods_get(i);
        known |= m->ptbr_matches != 0;
        complete |= m->ptbr_matches == PTBR_ALL;
        if (!m->on) continue;
        enabled |= m->ptbr_matches != 0;
        unsigned take = m->ptbr_paths & ~paths;
        paths |= take;
        matches |= m->ptbr_matches & take;
        code |= m->code && (m->ptbr_matches & take);
    }
    if (code) return PTBR_CODE;
    if (matches == PTBR_ALL) return PTBR_READY;
    if (!known) return PTBR_MISSING;
    if (!enabled) return PTBR_DISABLED;
    return complete ? PTBR_CONFLICT : PTBR_INCOMPLETE;
}

const char *localization_ptbr_message(void) {
    switch (localization_ptbr_status()) {
    case PTBR_READY:
        return "JumpManClub Brasil v1.1 detected. Portuguese uses the US English slot and the mod's font. "
               "Restart to apply. To restore original English, switch this mod off in Mods and restart.";
    case PTBR_DISABLED:
        return "Portuguese (Brazil): translation disabled. Enable the JumpManClub Brasil mod in Mods, then restart.";
    case PTBR_INCOMPLETE:
        return "Portuguese (Brazil): incomplete or modified translation. Install all four original v1.1 files, "
               "including FontData.szs, then restart. See docs/PT-BR.md.";
    case PTBR_CONFLICT:
        return "Portuguese (Brazil): another enabled mod overrides translation files. Check the alphabetical "
               "priority in Mods, disable the conflicting mod and restart.";
    case PTBR_CODE:
        return "Portuguese (Brazil): this package also contains unsupported code patches. Use the official "
               "RomFS-only Atmosphere v1.1 package and restart.";
    default:
        return "Portuguese (Brazil): translation not installed or unrecognized. Obtain JumpManClub Brasil v1.1 "
               "separately; see docs/PT-BR.md. The game uses the English slot until it is installed.";
    }
}
