#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

// Settings has no guest-CPU dependency; the platform redirects stderr to the host log.
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#include "input_bind.h"

#define SETTINGS_FILE settings_file()

// ODYSSEY_SETTINGS=<path>: another settings file (test runs never touch the player's powermoon.ini)
static const char *settings_file(void) {
    const char *e = getenv("ODYSSEY_SETTINGS");
    return e && *e ? e : "powermoon.ini";
}

Settings g_settings = {
    .fullscreen = false,
    .volume = 1.0f,
    .reverb = true,
    .movie_audio = true,
    .show_fps = false,
    .upscaler = UPSCALE_SHARP,  // (MetalFX on macOS, AMD FSR 1 on Windows)
    .motion = true, .rumble = true, .hd_rumble = true, .rumble_strength = 1.0f,
    .deband = true,
    .interpolation_output = 120,
};

// The game's own text languages: its RomFS carries these per region (regionUS/USen|USes|USfr, regionEU/EUen|EUes|...,
// regionJP/JPja, regionKR/KRkr, regionTW/TWch, regionCN/CNch), and the game picks one from the system language it
// reads back (runtime/services/am.c), exactly as it does on the console. Codes as the Switch reports them.
const char *const g_language_codes[N_LANGUAGES] = {"auto", "ja", "en-US", "es", "fr", "de", "it", "nl", "ru", "ko",
                                                   "zh-CN", "zh-TW"};
const char *const g_language_names[N_LANGUAGES] = {"Auto", "Japanese", "English", "Spanish", "French", "German",
                                                   "Italian", "Dutch", "Russian", "Korean", "Chinese (Simplified)",
                                                   "Chinese (Traditional)"};

// LANG_AUTO: the host's own language (LANG=es_ES.UTF-8 -> es), en-US when it is none of the game's. Linux only:
// macOS / Windows apps get no locale variables from the system, so Auto is English there (pick one in settings).
static const char *host_language(void) {
#ifndef __linux__
    return "en-US";
#endif
    const char *e = getenv("LC_ALL");
    if (!e || !*e) e = getenv("LC_MESSAGES");
    if (!e || !*e) e = getenv("LANG");
    if (!e || !*e || !strcmp(e, "C") || !strcmp(e, "POSIX")) return "en-US";
    char l[8] = {0};
    size_t n = 0;
    for (const char *p = e; *p && *p != '.' && *p != '@' && *p != '_' && n < sizeof l - 1; p++) l[n++] = *p;
    static const struct { const char *loc, *code; } map[] = {{"ja", "ja"}, {"ko", "ko"}, {"ru", "ru"}, {"nl", "nl"},
                                                             {"de", "de"}, {"it", "it"}, {"fr", "fr"}, {"es", "es"}};
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(l, map[i].loc)) return map[i].code;
    if (!strcmp(l, "zh")) return e[n] == '_' && e[n + 1] == 'T' ? "zh-TW" : "zh-CN";  // (TW / HK: traditional)
    return "en-US";
}

const char *settings_language(void) {
    int i = g_settings.language;
    return i > LANG_AUTO && i < N_LANGUAGES ? g_language_codes[i] : host_language();
}

const char *settings_language_name(void) {
    const char *c = settings_language();
    for (int i = 0; i < N_LANGUAGES; i++)
        if (!strcmp(c, g_language_codes[i])) return g_language_names[i];
    return "English";
}

static char **g_mod_off;  // the switched-off mods' folders (relative to mods/)
static int g_nmod_off;

bool settings_mod_off(const char *id) {
    for (int i = 0; i < g_nmod_off; i++)
        if (!strcmp(g_mod_off[i], id)) return true;
    return false;
}

void settings_set_mod_off(const char *id, bool off) {
    for (int i = 0; i < g_nmod_off; i++) {
        if (strcmp(g_mod_off[i], id)) continue;
        if (!off) { free(g_mod_off[i]); g_mod_off[i] = g_mod_off[--g_nmod_off]; }
        return;
    }
    if (!off) return;
    g_mod_off = realloc(g_mod_off, sizeof *g_mod_off * (g_nmod_off + 1));
    g_mod_off[g_nmod_off++] = strdup(id);
}

// Keep controller preferences outside the upstream settings writer. A stock upstream
// update can rewrite powermoon.ini without erasing this fork's motion/rumble choices.
static const char *controller_file(void) {
    static char path[1200];
    snprintf(path, sizeof path, "%s.controller.ini", settings_file());
    return path;
}

static bool controller_setting(const char *key, const char *val) {
    if (!strcmp(key,"motion")) g_settings.motion=atoi(val)!=0;
    else if (!strcmp(key,"rumble")) g_settings.rumble=atoi(val)!=0;
    else if (!strcmp(key,"hd_rumble")) g_settings.hd_rumble=atoi(val)!=0;
    else if (!strcmp(key,"rumble_strength")) {
        float v=(float)atof(val); g_settings.rumble_strength=v>=0 && v<=2?v:1;
    } else if (!bind_settings_load(key,val)) return false;
    return true;
}

static void controller_load(void) {
    FILE *f=fopen(controller_file(),"r");
    if (!f) return;
    char line[700],key[64],val[600];
    while (fgets(line,sizeof line,f)) {
        if (sscanf(line," %63[^= ] = %599[^\n]",key,val)<2) continue;
        for (size_t n=strlen(val); n && (val[n-1]==' ' || val[n-1]=='\r'); n--) val[n-1]=0;
        controller_setting(key,val);
    }
    fclose(f);
}

static void controller_save(void) {
    char temp[1250]; snprintf(temp,sizeof temp,"%s.tmp",controller_file());
    FILE *f=fopen(temp,"w");
    if (!f) { LOG("[settings] cannot save controller profile\n"); return; }
    fprintf(f,"# PowerMoon-Switch2Pro controller profile; survives stock upstream settings saves\n");
    fprintf(f,"motion = %d\nrumble = %d\nhd_rumble = %d\nrumble_strength = %.6g\n",
            g_settings.motion,g_settings.rumble,g_settings.hd_rumble,g_settings.rumble_strength);
    bind_settings_save(f);
    if (fclose(f)!=0) { remove(temp); return; }
#ifdef _WIN32
    if (!MoveFileExA(temp,controller_file(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        LOG("[settings] cannot replace controller profile\n");
#else
    if (rename(temp,controller_file())!=0) LOG("[settings] cannot replace controller profile\n");
#endif
}

void settings_load(void) {
    static bool loaded;
    if (loaded) return;
    loaded = true;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f && !getenv("ODYSSEY_SETTINGS")) f = fopen("odyssey.ini", "r");  // (the settings of the earlier builds)
    if (!f && !getenv("ODYSSEY_SETTINGS")) f = fopen("larecomp.ini", "r");
    if (!f) { controller_load(); controller_save(); return; }
    char line[700];
    while (fgets(line, sizeof line, f)) {
        char key[64];
        char val[600];  // (a mod's folder can be long)
        if (sscanf(line, " %63[^= ] = %599[^\n]", key, val) < 2) continue;
        for (size_t n = strlen(val); n && (val[n - 1] == ' ' || val[n - 1] == '\r'); n--) val[n - 1] = 0;
        if (!strcmp(key, "fullscreen")) g_settings.fullscreen = atoi(val) != 0;
        else if (!strcmp(key, "mod_off")) settings_set_mod_off(val, true);
        else if (!strcmp(key, "volume")) g_settings.volume = (float)atof(val);
        else if (!strcmp(key, "reverb")) g_settings.reverb = atoi(val) != 0;
        else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = atoi(val) != 0;
        else if (!strcmp(key, "show_fps")) g_settings.show_fps = atoi(val) != 0;
        else if (!strcmp(key, "motion")) g_settings.motion = atoi(val) != 0;
        else if (!strcmp(key, "rumble")) g_settings.rumble = atoi(val) != 0;
        else if (!strcmp(key, "hd_rumble")) g_settings.hd_rumble = atoi(val) != 0;
        else if (!strcmp(key, "rumble_strength")) { float v=(float)atof(val);g_settings.rumble_strength=v>=0 && v<=2?v:1; }
        else if (!strcmp(key, "resolution")) {
            int n = atoi(val);
            g_settings.res_mode = !strcmp(val, "dynamic") ? RES_DYNAMIC : !strcmp(val, "75%") ? RES_Q75 :
                                  !strcmp(val, "50%") ? RES_Q50 : !strcmp(val, "25%") ? RES_Q25 :
                                  (n >= 1 && n <= 4) ? n : n > 4 ? 4 : RES_MATCH;
        }
        else if (!strcmp(key, "upscaler")) g_settings.upscaler = !strcmp(val, "bilinear") ? UPSCALE_BILINEAR : UPSCALE_SHARP;  // (metalfx / fsr1: this OS's)
        else if (!strcmp(key, "antialias")) g_settings.antialias = !strcmp(val, "off") ? AA_OFF : AA_SMAA;
        else if (!strcmp(key, "deband")) g_settings.deband = atoi(val) != 0;
        else if (!strcmp(key, "aspect")) g_settings.aspect = !strcmp(val, "fill") ? ASPECT_FILL : ASPECT_16_9;
        else if (!strcmp(key,"interpolation_output")) g_settings.interpolation_output=!strcmp(val,"120") ? 120 : 0;
        else if (!strcmp(key,"vsync")) g_settings.vsync=!strcmp(val,"on") ? 1 : !strcmp(val,"off") ? 2 : 0;
        else if (!strcmp(key, "frame_interp")) g_settings.frame_interp = atoi(val) != 0;
        else if (!strcmp(key, "language")) {  // ("auto" or one of the game's codes; anything else: auto)
            g_settings.language = LANG_AUTO;
            for (int i = 1; i < N_LANGUAGES; i++)
                if (!strcmp(val, g_language_codes[i])) g_settings.language = i;
        }
        else bind_settings_load(key, val);  // bind_pad_* / bind_key_*: runtime/hle/bindings.c (others: ignored)
    }
    fclose(f);
    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;
    controller_load();
    controller_save();
    LOG("[settings] loaded; controller profile retained\n");
}

void settings_save(void) {
    controller_save();
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { LOG("[settings] cannot write %s\n", SETTINGS_FILE); return; }
    fprintf(f, "# Power Moon Recomp settings (edited from the in-game menu: F1, Esc, or the controller's Home button)\n");
    fprintf(f, "fullscreen = %d\n", g_settings.fullscreen);
    fprintf(f, "volume = %.2f\n", g_settings.volume);
    fprintf(f, "reverb = %d\n", g_settings.reverb);
    fprintf(f, "movie_audio = %d\n", g_settings.movie_audio);
    fprintf(f, "show_fps = %d\n", g_settings.show_fps);
    fprintf(f, "motion = %d\nrumble = %d\nrumble_strength = %.2f\n",g_settings.motion,g_settings.rumble,g_settings.rumble_strength);
    fprintf(f, "hd_rumble = %d\n",g_settings.hd_rumble);
    fprintf(f, "upscaler = %s\n", g_settings.upscaler == UPSCALE_METALFX ? "metalfx" : g_settings.upscaler == UPSCALE_FSR1 ? "fsr1" : "bilinear");
    fprintf(f, "antialias = %s\n", g_settings.antialias == AA_SMAA ? "smaa" : "off");
    fprintf(f, "deband = %d\n", g_settings.deband);
    fprintf(f, "aspect = %s\n", g_settings.aspect == ASPECT_FILL ? "fill" : "16:9");
    fprintf(f, "frame_interp = %d\n", g_settings.frame_interp);
    fprintf(f,"interpolation_output = %s\n",g_settings.interpolation_output==120 ? "120" : "auto");
    fprintf(f,"vsync = %s\n",g_settings.vsync==1 ? "on" : g_settings.vsync==2 ? "off" : "auto");
    if (g_settings.res_mode == RES_DYNAMIC) fprintf(f, "resolution = dynamic\n");
    else if (g_settings.res_mode == RES_Q75) fprintf(f, "resolution = 75%%\n");
    else if (g_settings.res_mode == RES_Q25) fprintf(f, "resolution = 25%%\n");
    else if (g_settings.res_mode == RES_Q50) fprintf(f, "resolution = 50%%\n");
    else if (g_settings.res_mode >= 1 && g_settings.res_mode <= 8) fprintf(f, "resolution = %dx\n", g_settings.res_mode);
    else fprintf(f, "resolution = match\n");
    fprintf(f, "language = %s\n", g_language_codes[g_settings.language > LANG_AUTO && g_settings.language < N_LANGUAGES
                                                     ? g_settings.language : LANG_AUTO]);
    for (int i = 0; i < g_nmod_off; i++) fprintf(f, "mod_off = %s\n", g_mod_off[i]);
    bind_settings_save(f);
    fclose(f);
}
