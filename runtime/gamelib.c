// Loads the recompiled game code library (runtime/gamelib.h).
#include "gamelib.h"
#include "gamelib_cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "paths.h"
#include "rt.h"

static const GameLibTables *g_tables;
static char g_error[256];

// The CPU the game code is compiled for. The Makefile's target is x86-64-v3 (AVX2, FMA, BMI2: Intel Haswell 2013+,
// AMD Excavator / Zen): on such a CPU nothing changes - the same flags, the same id. An older x86-64 CPU gets the game
// code compiled for what it has (the player's machine compiles it: runtime/gamebuild.c), and an id of its own, so
// code compiled for a newer CPU is rebuilt there rather than loaded (an illegal instruction). Without hardware FMA the
// Switch's fused multiply-adds stay exact (the C library's fma), only slower. ODYSSEY_CPU_LEVEL=v3 / avx / v2 / x64
// (tests): that level whatever the CPU has.
int gamelib_cpu_level(void) {
    static int level = -1;
    if (level >= 0) return level;
    const char *e = getenv("ODYSSEY_CPU_LEVEL");
    if (e && *e) {
        level = !strcmp(e, "avx") ? GAMELIB_CPU_AVX : !strcmp(e, "v2") ? GAMELIB_CPU_V2 : !strcmp(e, "x64") ? GAMELIB_CPU_X64
                                                                                                               : GAMELIB_CPU_V3;
        return level;
    }
#if defined(__x86_64__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2")) level = GAMELIB_CPU_V3;
    else if (__builtin_cpu_supports("avx") && __builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("popcnt")) level = GAMELIB_CPU_AVX;
    else if (__builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("popcnt") && __builtin_cpu_supports("ssse3")) level = GAMELIB_CPU_V2;
    else level = GAMELIB_CPU_X64;
#else
    level = GAMELIB_CPU_V3;  // (not x86: the build's own target)
#endif
    return level;
}

// The -march the game code is compiled with on this CPU; NULL: the build's own (GAMELIB_CFLAGS)
const char *gamelib_cpu_march(void) {
    switch (gamelib_cpu_level()) {
    case GAMELIB_CPU_AVX: return "sandybridge";  // (AVX, SSE4.2, POPCNT, AES, PCLMUL - no AVX2 / FMA / BMI)
    case GAMELIB_CPU_V2: return "x86-64-v2";
    case GAMELIB_CPU_X64: return "x86-64";
    default: return NULL;
    }
}

const char *gamelib_cpu_name(void) {
    switch (gamelib_cpu_level()) {
    case GAMELIB_CPU_AVX: return "AVX (no AVX2)";
    case GAMELIB_CPU_V2: return "SSE4.2 (no AVX)";
    case GAMELIB_CPU_X64: return "x86-64 baseline";
    default: return "AVX2";
    }
}

// GAMELIB_ID, with the CPU level when it isn't the build's own (-avx, -v2, -x64)
const char *gamelib_id(void) {
    static char id[96];
    if (!id[0]) {
        static const char *const suffix[] = {"", "-avx", "-v2", "-x64"};
        int l = gamelib_cpu_level();
        snprintf(id, sizeof id, "%s%s", GAMELIB_ID, l >= 0 && l < 4 ? suffix[l] : "");
    }
    return id;
}

// Is `id` this build's id? Also a library built from the same sources under an id made another way: {this build's
// id, the other}. Linux x86-64, 0.4.4 - 0.4.6: the id counted tools/zig_slim.sh, a script only Windows releases use
// (its change for #87 made every Steam Deck rebuild the game code, ~25 minutes, for the same library). Only while
// GAMELIB_ID is the first of a pair: any real change of the sources makes a new id and the pair no longer applies.
static bool id_same(const char *id) {
    if (!strcmp(id, gamelib_id())) return true;
    static const char *const same_as[][2] = {{"a1d43d90ca7ce979", "038563a1c29f433d"}};
    size_t base = strlen(GAMELIB_ID);
    for (size_t i = 0; i < sizeof same_as / sizeof same_as[0]; i++) {
        size_t n = strlen(same_as[i][1]);
        if (!strcmp(GAMELIB_ID, same_as[i][0]) && !strncmp(id, same_as[i][1], n) && !strcmp(id + n, gamelib_id() + base))
            return true;  // (the same CPU level's suffix)
    }
    return false;
}

// The library's id file (<library>.id, written next to it by the build): its gamelib_id, read without loading it.
static bool id_matches(const char *lib) {
    char path[1100], id[128] = {0};
    snprintf(path, sizeof path, "%s.id", lib);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(id, 1, sizeof id - 1, f);
    fclose(f);
    while (n && (id[n - 1] == '\n' || id[n - 1] == '\r')) id[--n] = 0;
    return id_same(id);
}

static bool exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// The places to look, in order (gamelib.h). Returns how many were filled.
static int candidates(const char *game_dir, char out[3][1100]) {
    int n = 0;
    const char *env = getenv("ODYSSEY_GAME_LIB");
    if (env && *env) snprintf(out[n++], 1100, "%s", env);
    char exe_side[1024];
    if (rt_resource(GAMELIB_NAME, NULL, exe_side, sizeof exe_side) && exists(exe_side))
        snprintf(out[n++], 1100, "%s", exe_side);
    if (game_dir) snprintf(out[n++], 1100, "%s/%s", game_dir, GAMELIB_NAME);
    return n;
}

bool gamelib_present(const char *game_dir) {
    if (g_tables) return true;
    char c[3][1100];
    int n = candidates(game_dir, c);
    for (int i = 0; i < n; i++)
        if (exists(c[i]) && id_matches(c[i])) return true;
    return false;
}

static const GameLibTables *open_lib(const char *path) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h) { snprintf(g_error, sizeof g_error, "can't load %s (error %lu)", path, GetLastError()); return NULL; }
    GameLibEntry entry = (GameLibEntry)(void *)GetProcAddress(h, GAMELIB_ENTRY);
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(g_error, sizeof g_error, "%s", dlerror()); return NULL; }
    GameLibEntry entry = (GameLibEntry)dlsym(h, GAMELIB_ENTRY);
#endif
    if (!entry) { snprintf(g_error, sizeof g_error, "%s: not game code", path); return NULL; }
    const GameLibTables *t = entry();
    if (!t || t->abi != GAMELIB_ABI || !t->id || !id_same(t->id)) {
        snprintf(g_error, sizeof g_error, "%s was built for another version", path);
        return NULL;  // (left loaded: nothing of it runs)
    }
    return t;
}

bool gamelib_load(const char *game_dir, char *why, size_t why_size) {
    static bool tried;
    if (!tried) {
        tried = true;
        snprintf(g_error, sizeof g_error, "the game code hasn't been built yet: `make gen`, then `make` (README.md)");
        char c[3][1100];
        int n = candidates(game_dir, c);
        const char *env = getenv("ODYSSEY_GAME_LIB");
        for (int i = 0; i < n && !g_tables; i++) {
            if (!exists(c[i])) continue;
            bool given = i == 0 && env && *env;  // ODYSSEY_GAME_LIB: loaded as given, the library's own id decides
            if (!given && !id_matches(c[i])) {
                snprintf(g_error, sizeof g_error, "%s was built for another version", c[i]);
                continue;
            }
            g_tables = open_lib(c[i]);
            if (g_tables) {
                char mods[160] = "";
                for (uint32_t k = 0; k < g_tables->nmodules; k++)
                    snprintf(mods + strlen(mods), sizeof mods - strlen(mods), "%s%s %llu", k ? ", " : "",
                             g_tables->modules[k].name, (unsigned long long)*g_tables->modules[k].count);
                LOG("[gamelib] %s (id %s; functions: %s)\n", c[i], g_tables->id, mods);
            }
        }
    }
    if (!g_tables && why) snprintf(why, why_size, "%s", g_error);
    return g_tables != NULL;
}

const GameLibTables *gamelib_tables(void) { return g_tables; }
