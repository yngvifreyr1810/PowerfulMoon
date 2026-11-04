// RomFS mods: finding them in mods/, their files for fs.c, on / off for the menu (see mods.h).
#include "mods.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hle.h"
#include "settings.h"

#define MODS_DIR "mods"
#define TITLE_ID "0100000000010000"  // Super Mario Odyssey: the console / emulator layouts name a folder after it

// Found once (the first time the game opens its RomFS, or the menu asks), kept for the whole run: a mod added while
// the game runs shows up at the next start.
static ModInfo *g_mods;
static int g_nmods;
static bool g_scanned, g_changed;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static bool is_dir(const char *p) {
    struct stat sb;
    return stat(p, &sb) == 0 && S_ISDIR(sb.st_mode);
}

// 16 hex digits: a game's folder in the console / emulator layouts
static bool title_id(const char *s) {
    int n = 0;
    for (; s[n]; n++)
        if (!isxdigit((unsigned char)s[n])) return false;
    return n == 16;
}

// "Luigi/atmosphere/contents/0100000000010000/romfs" -> "Luigi"; the loose files (romfs) -> "mods/romfs"
static void mod_name(const char *id, char *out, size_t cap) {
    out[0] = 0;
    for (const char *p = id; *p;) {
        const char *e = strchr(p, '/');
        int n = e ? (int)(e - p) : (int)strlen(p);
        char part[256];
        snprintf(part, sizeof part, "%.*s", n, p);
        if (strcasecmp(part, "romfs") && strcasecmp(part, "exefs") && strcasecmp(part, "atmosphere") &&
            strcasecmp(part, "contents") && !title_id(part)) {
            if (out[0]) strlcat(out, " / ", cap);
            strlcat(out, part, cap);
        }
        p += n + (e ? 1 : 0);
    }
    if (!out[0]) strlcpy(out, MODS_DIR "/romfs", cap);
}

static int count_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        n += is_dir(p) ? count_files(p) : 1;
    }
    closedir(d);
    return n;
}

static ModInfo *add(ModInfo **v, int *n, int *cap, const char *id) {
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 16; *v = realloc(*v, sizeof **v * *cap); }
    ModInfo *m = &(*v)[(*n)++];
    memset(m, 0, sizeof *m);
    strlcpy(m->id, id, sizeof m->id);
    mod_name(id, m->name, sizeof m->name);
    m->on = !settings_mod_off(id);
    return m;
}

// The romfs/ and exefs/ folders under mods/ (the console layout's are 5 levels down)
static void scan(const char *rel, int depth, ModInfo **v, int *n, int *cap) {
    char dirp[1100];
    snprintf(dirp, sizeof dirp, MODS_DIR "/%s", rel);
    DIR *d = opendir(dirp);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;  // (., .., hidden folders)
        char r2[512], h2[1100];
        snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(h2, sizeof h2, MODS_DIR "/%s", r2);
        if (!is_dir(h2)) continue;
        if (title_id(e->d_name) && strcasecmp(e->d_name, TITLE_ID)) continue;  // another game's
        if (!strcasecmp(e->d_name, "romfs")) {
            ModInfo *m = add(v, n, cap, r2);
            m->romfs = true;
            m->files = count_files(h2);
        } else if (!strcasecmp(e->d_name, "exefs")) {
            if (count_files(h2)) add(v, n, cap, r2)->code = true;  // (merged into its romfs/ mod below)
        } else if (depth < 6) {
            scan(r2, depth + 1, v, n, cap);
        }
    }
    closedir(d);
}

// the folder both a mod's romfs/ and exefs/ are in: "Luigi/romfs" -> "Luigi"
static bool same_parent(const char *a, const char *b) {
    const char *sa = strrchr(a, '/'), *sb = strrchr(b, '/');
    size_t la = sa ? (size_t)(sa - a) : 0, lb = sb ? (size_t)(sb - b) : 0;
    return la == lb && !strncasecmp(a, b, la);
}

static int mod_cmp(const void *a, const void *b) {
    const ModInfo *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcasecmp(x->id, y->id);
}

static void scan_once(void) {
    pthread_mutex_lock(&g_lock);
    if (!g_scanned) {
        g_scanned = true;
        settings_load();  // (which mods are switched off; loads only once)
        ModInfo *v = NULL;
        int n = 0, cap = 0;
        scan("", 0, &v, &n, &cap);
        // a mod with both: one entry (its romfs/), marked as having code patches too
        for (int i = 0; i < n; i++) {
            if (v[i].romfs || !v[i].code) continue;  // (an exefs/ entry)
            for (int j = 0; j < n; j++) {
                if (!v[j].romfs || !same_parent(v[i].id, v[j].id)) continue;
                v[j].code = true;
                memmove(v + i, v + i + 1, sizeof *v * (n - i - 1));
                n--, i--;
                break;
            }
        }
        if (n > 1) qsort(v, n, sizeof *v, mod_cmp);
        g_mods = v, g_nmods = n;
        for (int i = 0; i < n; i++)
            LOG("[mods] %s (mods/%s): %s, %d files%s\n", v[i].name, v[i].id, v[i].on ? "on" : "off", v[i].files,
                !v[i].romfs ? " - code patches only, can't be used" : v[i].code ? " (its code patches can't be used)" : "");
    }
    pthread_mutex_unlock(&g_lock);
}

int mods_count(void) {
    scan_once();
    return g_nmods;
}

const ModInfo *mods_get(int i) {
    return i >= 0 && i < mods_count() ? &g_mods[i] : NULL;
}

void mods_set_on(int i, bool on) {
    if (i < 0 || i >= mods_count() || g_mods[i].on == on) return;
    g_mods[i].on = on;
    g_changed = true;
    settings_set_mod_off(g_mods[i].id, !on);
    settings_save();
}

bool mods_changed(void) { return g_changed; }

int mods_applied(void) {
    int n = 0;
    for (int i = 0; i < g_nmods; i++) n += g_mods[i].applied;  // (no lock: also called from the crash notice)
    return n;
}

bool mods_dir_url(char *out, size_t cap) {
    mkdir(MODS_DIR, 0755);
    char cwd[1024];
    if (!getcwd(cwd, sizeof cwd)) return false;
    // file:///C:/Games/Power%20Moon%20Recomp/mods (Windows) or file:///home/me/.local/share/power-moon-recomp/mods
    size_t k = (size_t)snprintf(out, cap, "file://%s", cwd[0] == '/' ? "" : "/");
    for (const char *p = cwd; *p && k + 4 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\') out[k++] = '/';
        else if (c <= ' ' || c == '%' || c == '#' || c == '?' || c >= 0x80) k += (size_t)snprintf(out + k, cap - k, "%%%02X", c);
        else out[k++] = (char)c;
    }
    out[k] = 0;
    return strlcat(out, "/" MODS_DIR, cap) < cap;
}

// ---- the files of the switched-on mods, for fs.c

static void walk(int mod, const char *root, const char *rel, ModFile **v, int *n, int *cap) {
    char dirp[1400];
    snprintf(dirp, sizeof dirp, "%s%s%s", root, rel[0] ? "/" : "", rel);
    DIR *d = opendir(dirp);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char r2[1024], h2[1400];
        snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(h2, sizeof h2, "%s/%s", root, r2);
        struct stat sb;
        if (stat(h2, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) { walk(mod, root, r2, v, n, cap); continue; }
        if (*n == *cap) { *cap = *cap ? *cap * 2 : 256; *v = realloc(*v, sizeof **v * *cap); }
        (*v)[(*n)++] = (ModFile){strdup(r2), strdup(h2), (uint64_t)sb.st_size, mod, false};
    }
    closedir(d);
}

// (case-insensitive: a mod made on Windows may spell a folder differently from the game)
static int file_cmp(const void *a, const void *b) {
    const ModFile *x = a, *y = b;
    int c = strcasecmp(x->rel, y->rel);
    return c ? c : x->mod - y->mod;
}

int mods_files(ModFile **out) {
    scan_once();
    ModFile *v = NULL;
    int n = 0, cap = 0;
    for (int i = 0; i < g_nmods; i++) {
        ModInfo *m = &g_mods[i];
        m->replaced = m->added = m->shadowed = m->skipped = 0;
        m->applied = m->on && m->romfs;
        if (!m->applied) continue;
        char root[600];
        snprintf(root, sizeof root, MODS_DIR "/%s", m->id);
        walk(i, root, "", &v, &n, &cap);
    }
    if (n > 1) qsort(v, n, sizeof *v, file_cmp);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (k && !strcasecmp(v[k - 1].rel, v[i].rel)) {  // also in a mod before it in the list: that one's is used
            g_mods[v[i].mod].shadowed++;
            free(v[i].rel), free(v[i].host);
            continue;
        }
        v[k++] = v[i];
    }
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i].shadowed)
            LOG("[mods] %s: %d of its files are also in a mod before it in the list (those are used)\n", g_mods[i].name,
                g_mods[i].shadowed);
    *out = v;
    return k;
}

ModFile *mods_find(ModFile *v, int n, const char *rel) {
    ModFile key = {.rel = (char *)rel, .mod = -1};  // (mod -1: before any entry of that path)
    int lo = 0, hi = n;
    while (lo < hi) {  // the first entry not before the key
        int mid = (lo + hi) / 2;
        if (file_cmp(&v[mid], &key) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && !strcasecmp(v[lo].rel, rel) ? &v[lo] : NULL;
}

void mods_tally(int mod, int what) {
    if (mod < 0 || mod >= g_nmods) return;
    ModInfo *m = &g_mods[mod];
    if (what == MOD_REPLACED) m->replaced++;
    else if (what == MOD_ADDED) m->added++;
    else m->skipped++;
}

void mods_files_free(ModFile *v, int n) {
    for (int i = 0; i < n; i++) free(v[i].rel), free(v[i].host);
    free(v);
}
