// NSO loading, relocation, and symbol resolution.
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "romfs_read.h"
#include "odyssey_v141.h"

Module g_modules[4];
int g_nmodules;
Game g_game;

// main's build id (NSO header +0x40, first 8 bytes) -> the game
// dev: a version that doesn't run yet - only with ODYSSEY_DEV_VERSIONS=1 (players get the setup's and the start's "only
// 1.0.0" instead of a crash)
static const struct { Game game; const char *name; uint8_t id[8]; bool dev; } KNOWN_GAMES[] = {
    {GAME_ODYSSEY, "Super Mario Odyssey v1.0.0", {0x3c, 0xa1, 0x2d, 0xfa, 0xaf, 0x9c, 0x82, 0xda}, false},
    {GAME_ODYSSEY, "Super Mario Odyssey v1.4.1", {0xb4, 0x24, 0xbe, 0x15, 0x0a, 0x8e, 0x7d, 0x78}, false},
};
static bool known(unsigned i) { return !KNOWN_GAMES[i].dev || getenv("ODYSSEY_DEV_VERSIONS"); }

enum { DT_NULL = 0, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
       DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28 };
enum { R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027 };

typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Elf64Sym;
typedef struct { uint64_t off, info; int64_t addend; } Elf64Rela;

static size_t lz4_decompress(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen) {
    const uint8_t *s = src, *se = src + srclen;
    uint8_t *d = dst, *de = dst + dstlen;
    while (s < se) {
        uint8_t tok = *s++;
        size_t lit = tok >> 4;
        if (lit == 15) {
            uint8_t b;
            do { if (s == se) return 0; b = *s++; lit += b; } while (b == 255);
        }
        if (lit > (size_t)(de - d) || lit > (size_t)(se - s)) return 0;
        memcpy(d, s, lit);
        d += lit; s += lit;
        if (s >= se) break;
        if (se - s < 2) return 0;
        size_t off = s[0] | (s[1] << 8);
        s += 2;
        size_t ml = tok & 15;
        if (ml == 15) {
            uint8_t b;
            do { if (s == se) return 0; b = *s++; ml += b; } while (b == 255);
        }
        ml += 4;
        if (!off || off > (size_t)(d - dst) || ml > (size_t)(de - d)) return 0;
        uint8_t *m = d - off;
        while (ml--) *d++ = *m++;
    }
    return d - dst;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    long size = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    uint8_t *p = size > 0 && fseek(f, 0, SEEK_SET) == 0 ? malloc((size_t)size) : NULL;
    if (p && fread(p, 1, (size_t)size, f) != (size_t)size) { free(p); p = NULL; }
    fclose(f);
    if (p) *len = (size_t)size;
    return p;
}

bool nso_visit_segments(const char *path, RomfsFileVisitor visit, void *arg) {
    size_t size;
    uint8_t *raw = read_file(path, &size);
    if (!raw) return false;
    bool ok = size >= 0x100 && !memcmp(raw, "NSO0", 4);
    for (int i = 0; ok && i < 3; i++) {
        uint32_t offset, length, stored;
        memcpy(&offset, raw + 0x10 + 0x10 * i, sizeof offset);
        memcpy(&length, raw + 0x18 + 0x10 * i, sizeof length);
        bool compressed = (raw[0xc] & (1u << i)) != 0;
        if (!length) continue;
        stored = length;
        if (compressed) memcpy(&stored, raw + 0x60 + 4 * i, sizeof stored);
        if (offset > size || stored > size - offset || length > (512u << 20)) { ok = false; break; }
        if (compressed) {
            uint8_t *segment = malloc(length);
            ok = segment && lz4_decompress(raw + offset, stored, segment, length) == length && visit(segment, length, arg);
            free(segment);
        } else {
            ok = visit(raw + offset, length, arg);
        }
    }
    free(raw);
    return ok;
}

static void load_nso(Module *m, const char *path, uint64_t base) {
    size_t len;
    uint8_t *raw = read_file(path, &len);
    if (!raw || memcmp(raw, "NSO0", 4)) FATAL("cannot load %s\n", path);
    uint32_t flags = *(uint32_t *)(raw + 0xC);
    uint32_t seg[3][3];  // file_off, mem_off, size
    for (int i = 0; i < 3; i++) memcpy(seg[i], raw + 0x10 + i * 0x10, 12);
    uint32_t bss = *(uint32_t *)(raw + 0x3C);
    uint64_t size = (seg[2][1] + seg[2][2] + bss + 0x3FFF) & ~0x3FFFull;
    plat_commit(base, size);  // inside the guest window: zero-filled, read/write
    for (int i = 0; i < 3; i++) {
        uint32_t csize = *(uint32_t *)(raw + 0x60 + i * 4);
        uint8_t *dst = (uint8_t *)base + seg[i][1];
        if (flags & (1 << i)) {
            size_t got = lz4_decompress(raw + seg[i][0], csize, dst, seg[i][2]);
            if (got != seg[i][2]) FATAL("lz4 %s seg %d: %zu != %u\n", path, i, got, seg[i][2]);
        } else {
            memcpy(dst, raw + seg[i][0], seg[i][2]);
        }
    }
    memcpy(m->build_id, raw + 0x40, sizeof m->build_id);
    free(raw);
    m->base = base;
    m->size = size;
    m->text_off = seg[0][1]; m->text_size = seg[0][2];
    m->ro_off = seg[1][1];   m->ro_size = seg[1][2];
    m->data_off = seg[2][1]; m->data_size = seg[2][2] + bss;

    uint32_t mod0 = *(uint32_t *)(base + 4);
    uint64_t mod = base + mod0;
    m->dynamic = mod + *(int32_t *)(mod + 4);
    for (uint64_t *d = (uint64_t *)m->dynamic; d[0] != DT_NULL; d += 2) {
        switch (d[0]) {
        case DT_SYMTAB: m->symtab = base + d[1]; break;
        case DT_STRTAB: m->strtab = base + d[1]; break;
        case DT_HASH: m->nsyms = *(uint32_t *)(base + d[1] + 4); break;
        case 12: m->init = base + d[1]; break;  // DT_INIT
        case 13: m->fini = base + d[1]; break;  // DT_FINI
        case DT_INIT_ARRAY: m->init_array = base + d[1]; break;
        case DT_INIT_ARRAYSZ: m->init_array_sz = d[1]; break;
        case DT_FINI_ARRAY: m->fini_array = base + d[1]; break;
        case DT_FINI_ARRAYSZ: m->fini_array_sz = d[1]; break;
        }
    }
    LOG("loaded %-7s at %llx size %llx syms %llu\n", m->name, base, size, m->nsyms);
}

Module *loader_module(const char *name) {
    for (int i = 0; i < g_nmodules; i++)
        if (!strcmp(g_modules[i].name, name)) return &g_modules[i];
    return NULL;
}

// The game behind an NSO file, from its header alone (before anything is loaded): GAME_OTHER if unknown or unreadable.
Game loader_peek_game(const char *nso_path) {
    uint8_t h[0x48];
    FILE *f = fopen(nso_path, "rb");
    if (!f) return GAME_OTHER;
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n != sizeof h || memcmp(h, "NSO0", 4)) return GAME_OTHER;
    for (unsigned i = 0; i < sizeof KNOWN_GAMES / sizeof KNOWN_GAMES[0]; i++)
        if (!memcmp(h + 0x40, KNOWN_GAMES[i].id, sizeof KNOWN_GAMES[i].id) && known(i)) return KNOWN_GAMES[i].game;
    return GAME_OTHER;
}

static Game detect_game(const Module *main) {
    for (unsigned i = 0; i < sizeof KNOWN_GAMES / sizeof KNOWN_GAMES[0]; i++)
        if (!memcmp(main->build_id, KNOWN_GAMES[i].id, sizeof main->build_id) && known(i)) {
            LOG("game: %s\n", KNOWN_GAMES[i].name);
            return KNOWN_GAMES[i].game;
        }
    const uint8_t *b = main->build_id;
    LOG("game: unknown (main build id %02x%02x%02x%02x%02x%02x%02x%02x)\n", b[0], b[1], b[2], b[3], b[4], b[5], b[6],
        b[7]);
    return GAME_OTHER;
}

static Elf64Sym *sym(Module *m, uint64_t i) { return (Elf64Sym *)m->symtab + i; }
static const char *symname(Module *m, Elf64Sym *s) { return (const char *)(m->strtab + s->name); }

// Name -> address of every symbol the loaded modules define; the first module (in lookup order) defining a name wins,
// as in the search below. Odyssey's modules define 120k names: searching them one by one for each relocation took
// minutes. Built by loader_load once all modules are in; the names point into the modules' string tables.
typedef struct { const char *name; uint64_t addr; } SymSlot;
static SymSlot *g_symindex;
static uint64_t g_symmask;

static uint64_t name_hash(const char *s) {  // FNV-1a
    uint64_t h = 0xcbf29ce484222325ull;
    while (*s) h = (h ^ (uint8_t)*s++) * 0x100000001b3ull;
    return h;
}

static void build_symbol_index(void) {
    uint64_t n = 0, cap = 1;
    for (int mi = 0; mi < g_nmodules; mi++) n += g_modules[mi].nsyms;
    while (cap < 2 * n) cap <<= 1;
    g_symindex = calloc(cap, sizeof *g_symindex);
    if (!g_symindex) return;  // (the search below still works)
    g_symmask = cap - 1;
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        for (uint64_t i = 1; i < m->nsyms; i++) {
            Elf64Sym *s = sym(m, i);
            if (!s->shndx) continue;
            const char *nm = symname(m, s);
            uint64_t h = name_hash(nm) & g_symmask;
            while (g_symindex[h].name && strcmp(g_symindex[h].name, nm)) h = (h + 1) & g_symmask;
            if (!g_symindex[h].name) g_symindex[h] = (SymSlot){nm, m->base + s->value};
        }
    }
}

bool loader_main_is_v141(void) {
    static const uint8_t v141[8] = {0xb4, 0x24, 0xbe, 0x15, 0x0a, 0x8e, 0x7d, 0x78};
    const Module *main = loader_module("main");
    return main && !memcmp(main->build_id, v141, sizeof v141);
}

// 1.4.1's main has no symbols for the game's own functions: the matched addresses (runtime/odyssey_v141.h)
static uint64_t v141_symbol(const char *name) {
    if (!loader_main_is_v141()) return 0;
    for (size_t i = 0; i < sizeof ODYSSEY_V141_SYMS / sizeof ODYSSEY_V141_SYMS[0]; i++)
        if (!strcmp(ODYSSEY_V141_SYMS[i].name, name)) return ODYSSEY_V141_SYMS[i].addr;
    return 0;
}

uint64_t loader_find_symbol(const char *name) {
    if (g_symindex) {
        for (uint64_t h = name_hash(name) & g_symmask; g_symindex[h].name; h = (h + 1) & g_symmask)
            if (!strcmp(g_symindex[h].name, name)) return g_symindex[h].addr;
        return v141_symbol(name);
    }
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        for (uint64_t i = 1; i < m->nsyms; i++) {
            Elf64Sym *s = sym(m, i);
            if (s->shndx && !strcmp(symname(m, s), name)) return m->base + s->value;
        }
    }
    return v141_symbol(name);
}

const char *loader_symbol_at(uint64_t addr, uint64_t *offset) {
    const char *best = NULL;
    uint64_t best_addr = 0;
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        if (addr < m->base || addr >= m->base + m->size) continue;
        for (uint64_t i = 1; i < m->nsyms; i++) {
            Elf64Sym *s = sym(m, i);
            uint64_t a = m->base + s->value;
            if (s->shndx && (s->info & 0xF) == 2 && a <= addr && a > best_addr) {
                best = symname(m, s);
                best_addr = a;
            }
        }
    }
    if (offset) *offset = addr - best_addr;
    return best;
}

static uint64_t resolve(Module *m, uint32_t symidx) {
    Elf64Sym *s = sym(m, symidx);
    const char *name = symname(m, s);
    if (s->shndx) return m->base + s->value;
    HostFn impl = hle_lookup_impl(name);
    if (impl) return hle_register(name, impl);  // host implementation wins over the guest SDK's
    uint64_t a = loader_find_symbol(name);
    if (a) return a;
    if ((s->info >> 4) == 2) return 0;  // weak undefined
    return hle_register(name, NULL);
}

static void relocate(Module *m) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0;
    for (uint64_t *d = (uint64_t *)m->dynamic; d[0] != DT_NULL; d += 2) {
        if (d[0] == DT_RELA) rela = m->base + d[1];
        if (d[0] == DT_RELASZ) relasz = d[1];
        if (d[0] == DT_JMPREL) jmprel = m->base + d[1];
        if (d[0] == DT_PLTRELSZ) pltsz = d[1];
    }
    int unresolved = 0;
    for (int pass = 0; pass < 2; pass++) {
        Elf64Rela *r = (Elf64Rela *)(pass ? jmprel : rela);
        uint64_t n = (pass ? pltsz : relasz) / sizeof(Elf64Rela);
        for (uint64_t i = 0; i < n; i++) {
            uint64_t *where = (uint64_t *)(m->base + r[i].off);
            uint32_t type = (uint32_t)r[i].info, si = r[i].info >> 32;
            switch (type) {
            case R_RELATIVE: *where = m->base + r[i].addend; break;
            case R_ABS64:
            case R_GLOB_DAT:
            case R_JUMP_SLOT: {
                uint64_t s = resolve(m, si);
                *where = s ? s + r[i].addend : 0;
                if (s >= HLE_BASE && s < HLE_BASE + HLE_SIZE) unresolved++;
                break;
            }
            default: FATAL("%s: reloc type %u\n", m->name, type);
            }
        }
    }
    LOG("relocated %-5s (%d imports bound to host stubs)\n", m->name, unresolved);
}

void loader_load(const char *exefs) {
    char path[1024];
    free(g_symindex);  // (a reboot: its names were in the wiped modules)
    g_symindex = NULL;
    // in rtld's load (and symbol lookup) order; the bases are the ones `make gen` used (Makefile)
    static const struct { const char *name; uint64_t base; bool optional; } mods[] = {
        {"main", MAIN_BASE, false}, {"subsdk0", SUBSDK0_BASE, true}, {"sdk", SDK_BASE, false}};
    for (unsigned i = 0; i < sizeof mods / sizeof mods[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", exefs, mods[i].name);
        FILE *f = fopen(path, "rb");
        if (f) fclose(f);
        else if (mods[i].optional) continue;
        Module *m = &g_modules[g_nmodules++];
        m->name = mods[i].name;
        load_nso(m, path, mods[i].base);
    }
    g_game = detect_game(&g_modules[0]);
    build_symbol_index();
    for (int i = 0; i < g_nmodules; i++) relocate(&g_modules[i]);
}
