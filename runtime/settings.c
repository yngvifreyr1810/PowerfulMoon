#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "hle.h"
#include "input_bind.h"
#include "platform.h"

#define SETTINGS_FILE settings_file()

// The settings file: settings.ini in the data root (before 0.4.1: powermoon.ini, renamed at the first start - the
// name came from the project's first name). ODYSSEY_SETTINGS=<path>: another file (test runs never touch the player's).
static const char *settings_file(void) {
    const char *e = getenv("ODYSSEY_SETTINGS");
    if (e && *e) return e;
    static bool moved;
    if (!moved) {
        moved = true;
        FILE *now = fopen("settings.ini", "rb"), *old = now ? NULL : fopen("powermoon.ini", "rb");
        if (now) fclose(now);
        if (old) {
            fclose(old);
            if (rename("powermoon.ini", "settings.ini") == 0) LOG("[settings] powermoon.ini renamed to settings.ini\n");
            else LOG("[settings] couldn't rename powermoon.ini to settings.ini: reading and saving powermoon.ini\n");
        }
    }
    FILE *f = fopen("settings.ini", "rb");
    if (f) { fclose(f); return "settings.ini"; }
    f = fopen("powermoon.ini", "rb");  // (the rename failed: a read-only folder, the file in use)
    if (f) { fclose(f); return "powermoon.ini"; }
    return "settings.ini";
}
const char *settings_current_file(void) { return settings_file(); }

Settings g_settings = {
    .window_mode = BORDERLESS,  // (owner 2026-11-16: the first-start defaults, the same on Windows, Linux and the Steam Deck)
    .volume = 1.0f,
    .reverb = true,
    .movie_audio = true,
    .discord_presence = false,  // (off until the player turns it on - owner 2026-11-17)
    .show_fps = false,
    .overlay_size = 1, .overlay_opacity = 80,
    .overlay_gpu = true, .overlay_cpu = true, .overlay_ram = true, .overlay_graph = true, .overlay_hardware = true,
    .overlay_render = true, .overlay_position = true,
#ifdef __APPLE__
    .upscaler = UPSCALE_METALFX,
#else
    .upscaler = UPSCALE_BILINEAR,
#endif
    .dlss_sharpness = 20,
    .dlss_rcas_sharpness = 20,
    .temporal_off = true,
    .fsr3_mode = DLSS_QUALITY,
    .fsr3_sharpness = 20,
    .fsr3_cas_sharpness = 20,
    .fsr4_mode = DLSS_QUALITY, .fsr4_sharpness = 20, .fsr4_cas_sharpness = 20,
    .xess_mode = DLSS_QUALITY, .xess_sharpness = 20, .xess_cas_sharpness = 20,
    .fsr1_sharpness = 90,
    .deband = true,
    .hud_sharpen = true,
    .antialias = AA_FXAA,
    .rumble = true,
    .rumble_strength = 100,
    .motion = true,
    .motion_shake_pad = -1,
    .motion_shake_key = 514,  // (BIND_MOUSE_MIDDLE: the middle mouse button - nothing else uses it by default)
    .mouse_camera = true,
    .mouse_menus = true,
    // First start (owner, 2026-11-16): the Switch's docked setup (console_mode docked), its dynamic resolution off
    // (always the full resolution) and VSync off.
    .game_dynres = false,
    .vsync = VSYNC_OFF,
    .mouse_sensitivity = 100,
    .mouse_sensitivity_y = 100,
    .stick_camera_x = 100, .stick_camera_y = 100,
    .aniso = ANISO_GAME,  // (the game's own level, as on the Switch - owner 2026-11-16: the defaults look as the original)
    .frame_rate = FPS_60,
    .shader_preload = false,  // (it costs a lot of memory: nvn/vulkan/exec.c preload_room)
    .early_fences = 1,   // (on: nvn/objects.c nvnSyncWait; owner 2026-11-16: the speed-ups on by default, every platform)
    .record_thread = -1,  // (Auto: on with an NVIDIA GPU, the only kind it has been tested on - nvn/vulkan/exec.c dq_enabled)
    // (the Switch's controllers apply their own; 0 let a worn stick drift the map pointer, #22, #37, #106. 5% as Switch
    // emulators; it was 10%, then the owner asked for 5 with finer control, 2026-11-19)
    .stick_deadzone_l = 5, .stick_deadzone_r = 5,
    .stick_range_l = 100, .stick_range_r = 100,
    .motion_gyro_sens = 100,
    .motion_shake_threshold = 0,
    .skip_intro = true,
    .warmup = WARMUP_AUTO,  // (Long - every recorded pipeline ready before play - where the PC has the memory for it)
    .check_updates = true,
    .config_at_start = true,
};

volatile bool g_app_unfocused;

// The Steam Deck's first start (no settings file yet): the same settings as everywhere else, fitted to its screen - its
// own 1280 x 800 rendered and shown 1:1 (100%) and filled: the game's view made 16:10 (runtime/hle/odyssey_wide.c)
// instead of 16:9 with bars, the HUD at the picture's edges. (The battery saver's lighter settings - 75% with FSR 1,
// the game's handheld rendering - were the Deck's defaults before 2026-11-16; owner: one set of defaults.)
static void settings_steam_deck(void) {
    // (Multithreaded rendering off here: one scripted run in four with it on hung the Deck's GPU - "illegal register
    // access in command stream", a GPU reset - while never with it off; owner 2026-11-16: off on the Deck until found)
    g_settings.record_thread = -1;  // (Auto: off with its AMD GPU)
    g_settings.res_mode = RES_DISPLAY;
    g_settings.aspect = ASPECT_FILL;
    g_settings.hud_edges = 1;
}
bool settings_game_dynres(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("ODYSSEY_GAME_DYNRES");
        v = e && *e ? atoi(e) != 0 : g_settings.game_dynres;
    }
    return v;
}
bool settings_handheld(void) {
    static int h = -1;
    if (h < 0) {
        const char *e = getenv("ODYSSEY_HANDHELD");
        h = e && *e ? atoi(e) != 0 : g_settings.console_mode == CONSOLE_HANDHELD;
    }
    return h;
}
static const char *const warmups[WARMUP_COUNT] = {"auto", "normal", "long"};

void settings_warmup_budget(unsigned *ms, unsigned *mb) {
    int ram_mb = SDL_GetSystemRAM();
    // (Until 2026-11-17 Auto was Long with 32 GB or more. Each pipeline prepared ahead keeps ~1 MB of NVIDIA driver memory: Long's 7,650 cost
    // ~6 GB more in RAM than Normal's ~1,700 - on a 16 GB PC that meant swapping, #75 / #61; 2026-11-16)
    // (Auto since 2026-11-17: 1 GB on every PC - the first-scene pipelines and the most recent ones. 120 FPS routes through
    // six kingdoms, this PC: Long 8,925 pipelines and 14.7 GB in RAM at the start, Normal 1,883 and 11.3 GB, 1 GB 304
    // and 10.8 GB - FPS, hitches in play and at arrivals the same; none at all: more hitches. #94: Long took 32 GB PCs
    // to 10 GB in RAM at the start.)
    if (g_settings.warmup == WARMUP_AUTO) {
        *ms = 15000;
        *mb = 1024;
        return;
    }
    int w = g_settings.warmup;
    (void)ram_mb;
    if (w == WARMUP_LONG) {  // (what the 120 FPS launcher used: 30 s, up to 12 GB - at most half of this PC's memory)
        *ms = 30000;
        *mb = ram_mb > 0 && ram_mb / 2 < 12288 ? (unsigned)(ram_mb / 2) : 12288;
    } else {
        *ms = 15000;
        *mb = 3072;
    }
}

static const char *const dlss_modes[DLSS_MODE_COUNT] = {"off", "dlaa", "quality", "balanced", "performance"};
static const char *const dlss_presets[DLSS_PRESET_COUNT] = {"default", "k", "l", "m"};
static const char *const temporal_backends[TEMPORAL_BACKEND_COUNT] = {"dlss", "fsr3", "fsr4", "xess", "fsr2"};
static const char *const fsr3_modes[DLSS_MODE_COUNT] = {"off", "native", "quality", "balanced", "performance"};
static const char *const window_modes[WINDOW_MODE_COUNT] = {"windowed", "borderless", "exclusive"};
static const char *const display_modes[DISPLAY_MODE_COUNT] = {"auto", "1080p", "1440p", "2160p"};

static int named_option(const char *value, const char *const *options, int count) {
    for (int i = 0; i < count; i++) if (!strcmp(value, options[i])) return i;
    return 0;
}

static int g_aniso_start = -1;
int settings_aniso_at_start(void) {
    if (g_aniso_start < 0) {
        const char *e = getenv("ODYSSEY_ANISO");  // (tests: ODYSSEY_ANISO=0 / 2 / 4 / 8 / 16)
        g_aniso_start = e ? atoi(e) : g_settings.aniso;
        if (g_aniso_start != 2 && g_aniso_start != 4 && g_aniso_start != 8 && g_aniso_start != 16) g_aniso_start = 0;
    }
    return g_aniso_start;
}

// The game's own text languages: its RomFS carries these per region (regionUS/USen|USes|USfr, regionEU/EUen|EUes|...,
// regionJP/JPja, regionKR/KRkr, regionTW/TWch, regionCN/CNch), and the game picks one from the system language it
// reads back (runtime/services/am.c), exactly as it does on the console. Codes as the Switch reports them.
const char *const g_language_codes[N_LANGUAGES] = {"auto", "ja", "en-US", "es", "fr", "de", "it", "nl", "ru", "ko",
                                                   "zh-CN", "zh-TW", "pt-BR"};
const char *const g_language_names[N_LANGUAGES] = {"Auto", "Japanese", "English", "Spanish", "French", "German",
                                                   "Italian", "Dutch", "Russian", "Korean", "Chinese (Simplified)",
                                                   "Chinese (Traditional)", "Portugu\xc3\xaas (Brasil)"};

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
    // A traducao substitui USen; o jogo nao possui um idioma pt-BR nativo.
    if (i == LANG_PT_BR) return "en-US";
    return i > LANG_AUTO && i < N_LANGUAGES ? g_language_codes[i] : host_language();
}

const char *settings_language_name(void) {
    if (g_settings.language == LANG_PT_BR) return g_language_names[LANG_PT_BR];
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

void settings_load(void) {
    static bool loaded;
    if (loaded) return;
    loaded = true;
    // Steam Deck: fitted to its screen unless chosen otherwise (the settings file, read below, wins)
    if (plat_is_steam_deck()) settings_steam_deck();
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f && !getenv("ODYSSEY_SETTINGS")) f = fopen("odyssey.ini", "r");  // (the settings of the earlier builds)
    if (!f && !getenv("ODYSSEY_SETTINGS")) f = fopen("larecomp.ini", "r");
    if (!f) return;
    char line[700];
    bool mouse_vertical_set = false;
    while (fgets(line, sizeof line, f)) {
        char key[64];
        char val[600];  // (a mod's folder can be long)
        if (sscanf(line, " %63[^= ] = %599[^\n]", key, val) < 2) continue;
        for (size_t n = strlen(val); n && (val[n - 1] == ' ' || val[n - 1] == '\r'); n--) val[n - 1] = 0;
        if (!strcmp(key, "fullscreen")) g_settings.window_mode = atoi(val) ? BORDERLESS : WINDOWED;
        else if (!strcmp(key, "window_mode")) g_settings.window_mode = named_option(val, window_modes, WINDOW_MODE_COUNT);
        else if (!strcmp(key, "display_resolution")) {
            g_settings.display_resolution = named_option(val, display_modes, DISPLAY_MODE_COUNT);
            if (!g_settings.display_w && settings_display_height(g_settings.display_resolution))  // (the old choices)
                g_settings.display_h = settings_display_height(g_settings.display_resolution),
                g_settings.display_w = g_settings.display_h * 16 / 9;
        }
        else if (!strcmp(key, "display_size")) {
            int w = 0, h = 0;
            if (sscanf(val, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200 && w <= 16384 && h <= 16384)
                g_settings.display_w = w, g_settings.display_h = h;
            else g_settings.display_w = g_settings.display_h = 0;
        }
        else if (!strcmp(key, "refresh_rate")) { int r = atoi(val); g_settings.refresh_rate = r >= 0 && r <= 1000 ? r : 0; }
        else if (!strcmp(key, "vsync")) {  // (before 2026-11-16: "on" was mailbox - now Fast - and "strict" FIFO - now On)
            static const char *const v[VSYNC_COUNT] = {"on", "off", "strict", "adaptive"};
            g_settings.vsync = VSYNC_OFF;
            for (int i = 0; i < VSYNC_COUNT; i++) if (!strcmp(val, v[i])) g_settings.vsync = i;
        }
        else if (!strcmp(key, "vsync_mode")) {
            static const char *const v[VSYNC_COUNT] = {"fast", "off", "on", "adaptive"};
            g_settings.vsync = VSYNC_OFF;
            for (int i = 0; i < VSYNC_COUNT; i++) if (!strcmp(val, v[i])) g_settings.vsync = i;
        }
        else if (!strcmp(key, "monitor")) { int m = atoi(val); g_settings.monitor = m >= 0 && m <= 64 ? m : 0; }
        else if (!strcmp(key, "mod_off")) settings_set_mod_off(val, true);
        else if (!strcmp(key, "volume")) g_settings.volume = (float)atof(val);
        else if (!strcmp(key, "reverb")) g_settings.reverb = atoi(val) != 0;
        else if (!strcmp(key, "audio_output"))
            g_settings.audio_output = !strcmp(val, "stereo") ? AUDIO_OUT_STEREO : !strcmp(val, "5.1") ? AUDIO_OUT_SURROUND : AUDIO_OUT_AUTO;
        else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = atoi(val) != 0;
        else if (!strcmp(key, "show_fps")) g_settings.show_fps = atoi(val) != 0;
        else if (!strcmp(key, "performance_overlay")) g_settings.performance_overlay = atoi(val) != 0;
        else if (!strcmp(key, "overlay_corner")) { int v = atoi(val); g_settings.overlay_corner = v >= 0 && v < OVERLAY_CORNER_COUNT ? v : 0; }
        else if (!strcmp(key, "overlay_size")) { int v = atoi(val); g_settings.overlay_size = v >= 0 && v <= 2 ? v : 1; }
        else if (!strcmp(key, "overlay_opacity")) { int v = atoi(val); g_settings.overlay_opacity = v < 0 ? 0 : v > 100 ? 100 : v; }
        else if (!strcmp(key, "overlay_gpu")) g_settings.overlay_gpu = atoi(val) != 0;
        else if (!strcmp(key, "overlay_cpu")) g_settings.overlay_cpu = atoi(val) != 0;
        else if (!strcmp(key, "overlay_ram")) g_settings.overlay_ram = atoi(val) != 0;
        else if (!strcmp(key, "overlay_graph")) g_settings.overlay_graph = atoi(val) != 0;
        else if (!strcmp(key, "overlay_hardware")) g_settings.overlay_hardware = atoi(val) != 0;
        else if (!strcmp(key, "overlay_render")) g_settings.overlay_render = atoi(val) != 0;
        else if (!strcmp(key, "overlay_position")) g_settings.overlay_position = atoi(val) != 0;
        else if (!strcmp(key, "vibration")) g_settings.rumble = atoi(val) != 0;
        else if (!strcmp(key, "vibration_strength")) { int v = atoi(val); g_settings.rumble_strength = v >= 25 && v <= 200 ? v : 100; }
        else if (!strcmp(key, "motion")) g_settings.motion = atoi(val) != 0;
        else if (!strcmp(key, "motion_filter")) { int v = atoi(val); g_settings.motion_filter = v >= 0 && v <= 2 ? v : 0; }
        else if (!strcmp(key, "motion_invert_x")) g_settings.motion_invert_x = atoi(val) != 0;
        else if (!strcmp(key, "motion_invert_y")) g_settings.motion_invert_y = atoi(val) != 0;
        else if (!strcmp(key, "motion_gyro_bias")) {
            float b[3];
            if (sscanf(val, "%f %f %f", &b[0], &b[1], &b[2]) == 3 && fabsf(b[0]) < 0.1f && fabsf(b[1]) < 0.1f && fabsf(b[2]) < 0.1f)
                memcpy(g_settings.motion_gyro_bias, b, sizeof b);
        }
        else if (!strcmp(key, "motion_shake_pad")) { int v = atoi(val); g_settings.motion_shake_pad = v >= -1 && v < 64 ? v : -1; }
        else if (!strcmp(key, "motion_shake_key")) { int v = atoi(val); g_settings.motion_shake_key = v >= 0 && v < 1024 ? v : 0; }
        else if (!strcmp(key, "stick_deadzone")) {  // (before 0.4.11: one for both sticks. 0 and 10 were the defaults
            // written by older versions - 0 is what let the map pointer drift (#106) - so they become the new default)
            int v = atoi(val) < 0 ? 0 : atoi(val) > 50 ? 50 : atoi(val);
            if (v == 0 || v == 10) v = 5;
            g_settings.stick_deadzone_l = g_settings.stick_deadzone_r = v;
        }
        else if (!strcmp(key, "stick_deadzone_left")) g_settings.stick_deadzone_l = atoi(val) < 0 ? 0 : atoi(val) > 50 ? 50 : atoi(val);
        else if (!strcmp(key, "stick_deadzone_right")) g_settings.stick_deadzone_r = atoi(val) < 0 ? 0 : atoi(val) > 50 ? 50 : atoi(val);
        else if (!strcmp(key, "stick_range_left")) g_settings.stick_range_l = atoi(val) < 50 ? 50 : atoi(val) > 150 ? 150 : atoi(val);
        else if (!strcmp(key, "stick_range_right")) g_settings.stick_range_r = atoi(val) < 50 ? 50 : atoi(val) > 150 ? 150 : atoi(val);
        else if (!strcmp(key, "motion_gyro_sensitivity")) g_settings.motion_gyro_sens = atoi(val) < 25 ? 25 : atoi(val) > 400 ? 400 : atoi(val);
        else if (!strcmp(key, "motion_shake_threshold")) g_settings.motion_shake_threshold = atoi(val) < 0 ? 0 : atoi(val) > 20 ? 20 : atoi(val);
        else if (!strcmp(key, "resolution")) {
            int n = atoi(val);
            g_settings.res_mode = !strcmp(val, "dynamic") ? RES_DYNAMIC : !strcmp(val, "75%") ? RES_Q75 :
                                  !strcmp(val, "50%") ? RES_Q50 : !strcmp(val, "25%") ? RES_Q25 : !strcmp(val, "1440p") ? RES_1440 : !strcmp(val, "100%") ? RES_DISPLAY :
                                  (n >= 1 && n <= 4) ? n : n > 4 ? 4 : RES_MATCH;
        }
        else if (!strcmp(key, "upscaler")) g_settings.upscaler = !strcmp(val, "bilinear") ? UPSCALE_BILINEAR : UPSCALE_SHARP;  // (metalfx / fsr1: this OS's)
        else if (!strcmp(key, "antialias")) g_settings.antialias = !strcmp(val, "off") ? AA_OFF : !strcmp(val, "fxaa") ? AA_FXAA : AA_SMAA;
        else if (!strcmp(key, "dlss_mode")) g_settings.dlss_mode = named_option(val, dlss_modes, DLSS_MODE_COUNT);
        else if (!strcmp(key, "dlss_preset")) g_settings.dlss_preset = named_option(val, dlss_presets, DLSS_PRESET_COUNT);
        else if (!strcmp(key, "temporal_backend")) g_settings.temporal_backend = named_option(val, temporal_backends, TEMPORAL_BACKEND_COUNT);
        else if (!strcmp(key, "upscale_method")) g_settings.temporal_off = !strcmp(val, "off");
        else if (!strcmp(key, "frame_generation")) g_settings.frame_gen = atoi(val) ? FG_FSR : FG_OFF;  // (0.2.12 dev)
        else if (!strcmp(key, "frame_gen")) g_settings.frame_gen = !strcmp(val, "fsr") ? FG_FSR : !strcmp(val, "dlss") ? FG_DLSS : FG_OFF;
        else if (!strcmp(key, "hud_position")) g_settings.hud_edges = strcmp(val, "centered") != 0;
        else if (!strcmp(key, "low_latency")) { int v = atoi(val); g_settings.low_latency = v >= 0 && v <= 2 ? v : 0; }
        else if (!strcmp(key, "fsr3_mode")) g_settings.fsr3_mode = named_option(val, fsr3_modes, DLSS_MODE_COUNT);
        else if (!strcmp(key, "fsr4_mode")) g_settings.fsr4_mode = named_option(val, fsr3_modes, DLSS_MODE_COUNT);
        else if (!strcmp(key, "xess_mode")) g_settings.xess_mode = named_option(val, fsr3_modes, DLSS_MODE_COUNT);
        else if (!strcmp(key, "fsr4_cas")) g_settings.fsr4_cas = atoi(val) != 0;
        else if (!strcmp(key, "xess_cas")) g_settings.xess_cas = atoi(val) != 0;
        else if (!strcmp(key, "fsr4_sharpness") || !strcmp(key, "fsr4_cas_sharpness") ||
                 !strcmp(key, "xess_sharpness") || !strcmp(key, "xess_cas_sharpness")) {
            int n = atoi(val); n = n < 0 ? 0 : n > 100 ? 100 : n;
            if (!strcmp(key, "fsr4_sharpness")) g_settings.fsr4_sharpness = n;
            else if (!strcmp(key, "fsr4_cas_sharpness")) g_settings.fsr4_cas_sharpness = n;
            else if (!strcmp(key, "xess_sharpness")) g_settings.xess_sharpness = n;
            else g_settings.xess_cas_sharpness = n;
        }
        else if (!strcmp(key, "fsr3_cas")) g_settings.fsr3_cas = atoi(val) != 0;
        else if (!strcmp(key, "fsr3_sharpness") || !strcmp(key, "fsr3_cas_sharpness")) {
            int n = atoi(val); n = n < 0 ? 0 : n > 100 ? 100 : n;
            if (!strcmp(key, "fsr3_sharpness")) g_settings.fsr3_sharpness = n;
            else g_settings.fsr3_cas_sharpness = n;
        }
        else if (!strcmp(key, "dlss_cas")) g_settings.dlss_cas = atoi(val) != 0;
        else if (!strcmp(key, "dlss_sharpness")) {
            int n = atoi(val);
            g_settings.dlss_sharpness = n < 0 ? 0 : n > 100 ? 100 : n;
        }
        else if (!strcmp(key, "fsr1_sharpness")) {
            int n = atoi(val);
            g_settings.fsr1_sharpness = n < 0 ? 0 : n > 100 ? 100 : n;
        }
        else if (!strcmp(key, "dlss_rcas_sharpness")) {
            int n = atoi(val);
            g_settings.dlss_rcas_sharpness = n < 0 ? 0 : n > 100 ? 100 : n;
        }
        else if (!strcmp(key, "deband")) g_settings.deband = atoi(val) != 0;
        else if (!strcmp(key, "hud_sharpen")) g_settings.hud_sharpen = atoi(val) != 0;
        else if (!strcmp(key, "aspect")) {
            g_settings.aspect = ASPECT_16_9;
            for (int i = 0; i < ASPECT_COUNT; i++) if (!strcmp(val, ASPECT_NAMES[i])) g_settings.aspect = i;
        }
        else if (!strcmp(key, "console_mode")) g_settings.console_mode = !strcmp(val, "handheld") ? CONSOLE_HANDHELD : CONSOLE_DOCKED;
        else if (!strcmp(key, "game_dynres")) g_settings.game_dynres = atoi(val) != 0;
        else if (!strcmp(key, "frame_interp")) {}  // (120 FPS interpolation: removed 2026-11-16 - it crashed; an old "1" is ignored)
        else if (!strcmp(key, "aniso")) {
            int n = atoi(val);
            g_settings.aniso = n == 2 || n == 4 || n == 8 || n == 16 ? n : ANISO_GAME;  // ("game")
        }
        else if (!strcmp(key, "shader_preload")) g_settings.shader_preload = atoi(val) != 0;
        else if (!strcmp(key, "shader_known_only")) g_settings.shader_known_only = atoi(val) != 0;
        else if (!strcmp(key, "shadow_quality")) g_settings.shadow_quality = atoi(val) < 0 ? 0 : atoi(val) > 2 ? 2 : atoi(val);
        else if (!strcmp(key, "bloom_quality")) g_settings.bloom_quality = !strcmp(val, "high") || atoi(val) > 0;
        else if (!strcmp(key, "draw_distance")) g_settings.draw_distance = atoi(val) < 0 ? 0 : atoi(val) > 2 ? 2 : atoi(val);
        // (on / off; "auto" and the older 0 / 1 were the earlier defaults - now on, as the default is)
        else if (!strcmp(key, "early_fences")) g_settings.early_fences = strcmp(val, "off") != 0;
        else if (!strcmp(key, "record_thread")) g_settings.record_thread = !strcmp(val, "on") ? 1 : !strcmp(val, "off") ? 0 : -1;
        else if (!strcmp(key, "texture_detail")) g_settings.texture_detail = atoi(val) < 0 ? 0 : atoi(val) > 2 ? 2 : atoi(val);
        else if (!strcmp(key, "memory"))
            g_settings.memory = !strcmp(val, "normal") ? MEMORY_NORMAL : !strcmp(val, "low") ? MEMORY_LOW : MEMORY_AUTO;
        else if (!strcmp(key, "shader_wait")) g_settings.shader_wait = atoi(val) != 0;
        else if (!strcmp(key, "frame_rate")) {  // (frames per second; another number: the nearest offered)
            int want = atoi(val), best = FPS_60;
            if (!strcmp(val, "display")) want = -1, best = FPS_AUTO;
            for (int i = 0; i < FPS_AUTO && want >= 0; i++)
                if (abs(settings_fps_hz(i) - want) < abs(settings_fps_hz(best) - want)) best = i;
            g_settings.frame_rate = best;
        }
        else if (!strcmp(key, "skip_intro")) g_settings.skip_intro = atoi(val) != 0;
        else if (!strcmp(key, "warmup")) g_settings.warmup = named_option(val, warmups, WARMUP_COUNT);
        else if (!strcmp(key, "mute_unfocused")) g_settings.mute_unfocused = atoi(val) != 0;
        else if (!strcmp(key, "discord_presence")) g_settings.discord_presence = atoi(val) != 0;
        else if (!strcmp(key, "check_updates")) g_settings.check_updates = atoi(val) != 0;
        else if (!strcmp(key, "config_at_start")) g_settings.config_at_start = atoi(val) != 0;
        else if (!strcmp(key, "mouse_camera")) g_settings.mouse_camera = atoi(val) != 0;
        else if (!strcmp(key, "mouse_menus")) g_settings.mouse_menus = atoi(val) != 0;
        else if (!strcmp(key, "mouse_sensitivity")) {
            g_settings.mouse_sensitivity = atoi(val) < 25 ? 25 : atoi(val) > 400 ? 400 : atoi(val);
            if (!mouse_vertical_set) g_settings.mouse_sensitivity_y = g_settings.mouse_sensitivity;
        }
        else if (!strcmp(key, "mouse_sensitivity_y")) {
            g_settings.mouse_sensitivity_y = atoi(val) < 25 ? 25 : atoi(val) > 400 ? 400 : atoi(val);
            mouse_vertical_set = true;
        }
        else if (!strcmp(key, "mouse_invert_x")) g_settings.mouse_invert_x = atoi(val) != 0;
        else if (!strcmp(key, "mouse_invert_y")) g_settings.mouse_invert_y = atoi(val) != 0;
        else if (!strcmp(key, "stick_camera_x"))
            g_settings.stick_camera_x = atoi(val) < 25 ? 25 : atoi(val) > 400 ? 400 : atoi(val);
        else if (!strcmp(key, "stick_camera_y"))
            g_settings.stick_camera_y = atoi(val) < 25 ? 25 : atoi(val) > 400 ? 400 : atoi(val);
        else if (!strcmp(key, "stick_camera_invert_x")) g_settings.stick_camera_invert_x = atoi(val) != 0;
        else if (!strcmp(key, "stick_camera_invert_y")) g_settings.stick_camera_invert_y = atoi(val) != 0;
        else if (!strcmp(key, "button_prompts"))
            g_settings.prompts = !strcmp(val, "controller") ? PROMPTS_CONTROLLER : !strcmp(val, "keyboard") ? PROMPTS_KEYBOARD :
                                 !strcmp(val, "switch") ? PROMPTS_SWITCH : !strcmp(val, "xbox") ? PROMPTS_XBOX :
                                 !strcmp(val, "playstation") ? PROMPTS_PLAYSTATION : PROMPTS_AUTO;
        else if (!strcmp(key, "language")) {  // ("auto" or one of the game's codes; anything else: auto)
            g_settings.language = LANG_AUTO;
            for (int i = 1; i < N_LANGUAGES; i++)
                if (!strcmp(val, g_language_codes[i])) g_settings.language = i;
        }
        else if (!strcmp(key, "menu_language"))
        {
            g_settings.menu_language = MENU_LANG_AUTO;
            const char *code = !strcmp(val, "en") ? "en-US" : val;  // (the earlier values: "en", "ko")
            for (int i = 1; i < N_LANGUAGES; i++)
                if (!strcmp(code, g_language_codes[i])) g_settings.menu_language = i;
        }
        else bind_settings_load(key, val);  // bind_pad_* / bind_key_*: runtime/hle/bindings.c (others: ignored)
    }
    fclose(f);
    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;
    settings_aniso_at_start();  // (latched now: the menu's changes apply at the next start)
    LOG("[settings] loaded %s (show at start %d)\n", SETTINGS_FILE, g_settings.config_at_start);
}

void settings_save(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { LOG("[settings] cannot write %s\n", SETTINGS_FILE); return; }
    fprintf(f, "# Super Odyssey PC Recomp settings (edited from the in-game menu: F1, Esc, or the controller's Home button)\n");
    fprintf(f, "window_mode = %s\n", window_modes[g_settings.window_mode]);
    fprintf(f, "display_resolution = %s\n", display_modes[g_settings.display_resolution]);
    if (g_settings.display_w) fprintf(f, "display_size = %dx%d\n", g_settings.display_w, g_settings.display_h);
    else fprintf(f, "display_size = auto\n");
    fprintf(f, "refresh_rate = %d\n", g_settings.refresh_rate);
    {
        static const char *const v[VSYNC_COUNT] = {"fast", "off", "on", "adaptive"};
        fprintf(f, "vsync_mode = %s\n", v[g_settings.vsync >= 0 && g_settings.vsync < VSYNC_COUNT ? g_settings.vsync : VSYNC_OFF]);
    }
    fprintf(f, "monitor = %d\n", g_settings.monitor);
    fprintf(f, "volume = %.2f\n", g_settings.volume);
    fprintf(f, "reverb = %d\n", g_settings.reverb);
    fprintf(f, "audio_output = %s\n", g_settings.audio_output == AUDIO_OUT_STEREO ? "stereo" :
                                      g_settings.audio_output == AUDIO_OUT_SURROUND ? "5.1" : "auto");
    fprintf(f, "movie_audio = %d\n", g_settings.movie_audio);
    fprintf(f, "show_fps = %d\n", g_settings.show_fps);
    fprintf(f, "performance_overlay = %d\n", g_settings.performance_overlay);
    fprintf(f, "overlay_corner = %d\noverlay_size = %d\noverlay_opacity = %d\n", g_settings.overlay_corner,
            g_settings.overlay_size, g_settings.overlay_opacity);
    fprintf(f, "overlay_gpu = %d\noverlay_cpu = %d\noverlay_ram = %d\noverlay_graph = %d\n", g_settings.overlay_gpu,
            g_settings.overlay_cpu, g_settings.overlay_ram, g_settings.overlay_graph);
    fprintf(f, "overlay_hardware = %d\noverlay_render = %d\noverlay_position = %d\n", g_settings.overlay_hardware,
            g_settings.overlay_render, g_settings.overlay_position);
    fprintf(f, "vibration = %d\n", g_settings.rumble);
    fprintf(f, "vibration_strength = %d\n", g_settings.rumble_strength);
    fprintf(f, "motion = %d\n", g_settings.motion);
    fprintf(f, "motion_filter = %d\n", g_settings.motion_filter);
    fprintf(f, "motion_invert_x = %d\n", g_settings.motion_invert_x);
    fprintf(f, "motion_invert_y = %d\n", g_settings.motion_invert_y);
    fprintf(f, "motion_gyro_bias = %.5f %.5f %.5f\n", g_settings.motion_gyro_bias[0], g_settings.motion_gyro_bias[1],
            g_settings.motion_gyro_bias[2]);
    fprintf(f, "motion_shake_pad = %d\n", g_settings.motion_shake_pad);
    fprintf(f, "motion_shake_key = %d\n", g_settings.motion_shake_key);
    fprintf(f, "stick_deadzone_left = %d\n", g_settings.stick_deadzone_l);
    fprintf(f, "stick_deadzone_right = %d\n", g_settings.stick_deadzone_r);
    fprintf(f, "stick_range_left = %d\n", g_settings.stick_range_l);
    fprintf(f, "stick_range_right = %d\n", g_settings.stick_range_r);
    fprintf(f, "motion_gyro_sensitivity = %d\n", g_settings.motion_gyro_sens);
    fprintf(f, "motion_shake_threshold = %d\n", g_settings.motion_shake_threshold);
    fprintf(f, "upscaler = %s\n", g_settings.upscaler == UPSCALE_METALFX ? "metalfx" : g_settings.upscaler == UPSCALE_FSR1 ? "fsr1" : "bilinear");
    fprintf(f, "antialias = %s\n", g_settings.antialias == AA_SMAA ? "smaa" : g_settings.antialias == AA_FXAA ? "fxaa" : "off");
    fprintf(f, "dlss_mode = %s\n", dlss_modes[g_settings.dlss_mode]);
    fprintf(f, "dlss_preset = %s\n", dlss_presets[g_settings.dlss_preset]);
    fprintf(f, "upscale_method = %s\n", g_settings.temporal_off ? "off" : temporal_backends[g_settings.temporal_backend]);
    fprintf(f, "frame_gen = %s\n", g_settings.frame_gen == FG_FSR ? "fsr" : g_settings.frame_gen == FG_DLSS ? "dlss" : "off");
    fprintf(f, "low_latency = %d\n", g_settings.low_latency);
    fprintf(f, "hud_position = %s\n", g_settings.hud_edges ? "edges" : "centered");
    fprintf(f, "temporal_backend = %s\n", temporal_backends[g_settings.temporal_backend]);
    fprintf(f, "fsr3_mode = %s\n", fsr3_modes[g_settings.fsr3_mode]);
    fprintf(f, "fsr3_sharpness = %d\n", g_settings.fsr3_sharpness);
    fprintf(f, "fsr3_cas = %d\n", g_settings.fsr3_cas);
    fprintf(f, "fsr3_cas_sharpness = %d\n", g_settings.fsr3_cas_sharpness);
    fprintf(f, "fsr4_mode = %s\nfsr4_sharpness = %d\nfsr4_cas = %d\nfsr4_cas_sharpness = %d\n",
        fsr3_modes[g_settings.fsr4_mode], g_settings.fsr4_sharpness, g_settings.fsr4_cas, g_settings.fsr4_cas_sharpness);
    fprintf(f, "xess_mode = %s\nxess_sharpness = %d\nxess_cas = %d\nxess_cas_sharpness = %d\n",
        fsr3_modes[g_settings.xess_mode], g_settings.xess_sharpness, g_settings.xess_cas, g_settings.xess_cas_sharpness);
    fprintf(f, "dlss_cas = %d\n", g_settings.dlss_cas);
    fprintf(f, "dlss_sharpness = %d\n", g_settings.dlss_sharpness);
    fprintf(f, "dlss_rcas_sharpness = %d\n", g_settings.dlss_rcas_sharpness);
    fprintf(f, "fsr1_sharpness = %d\n", g_settings.fsr1_sharpness);
    fprintf(f, "deband = %d\n", g_settings.deband);
    fprintf(f, "hud_sharpen = %d\n", g_settings.hud_sharpen);
    fprintf(f, "aspect = %s\n", ASPECT_NAMES[g_settings.aspect >= 0 && g_settings.aspect < ASPECT_COUNT ? g_settings.aspect : 0]);
    fprintf(f, "console_mode = %s\n", g_settings.console_mode == CONSOLE_HANDHELD ? "handheld" : "docked");
    fprintf(f, "game_dynres = %d\n", g_settings.game_dynres);
    if (g_settings.aniso) fprintf(f, "aniso = %d\n", g_settings.aniso);
    else fprintf(f, "aniso = game\n");
    if (g_settings.frame_rate == FPS_AUTO) fprintf(f, "frame_rate = display\n");
    else fprintf(f, "frame_rate = %d\n", settings_fps_hz(g_settings.frame_rate));
    fprintf(f, "shader_preload = %d\n", g_settings.shader_preload);
    fprintf(f, "shader_known_only = %d\n", g_settings.shader_known_only);
    fprintf(f, "shadow_quality = %d\n", g_settings.shadow_quality);
    fprintf(f, "bloom_quality = %s\n", g_settings.bloom_quality ? "high" : "switch");
    fprintf(f, "draw_distance = %d\n", g_settings.draw_distance);
    fprintf(f, "texture_detail = %d\n", g_settings.texture_detail);
    fprintf(f, "early_fences = %s\n", g_settings.early_fences ? "on" : "off");
    fprintf(f, "record_thread = %s\n", g_settings.record_thread > 0 ? "on" : g_settings.record_thread == 0 ? "off" : "auto");
    fprintf(f, "memory = %s\n", g_settings.memory == MEMORY_NORMAL ? "normal" : g_settings.memory == MEMORY_LOW ? "low" : "auto");
    fprintf(f, "shader_wait = %d\n", g_settings.shader_wait);
    fprintf(f, "skip_intro = %d\n", g_settings.skip_intro);
    fprintf(f, "warmup = %s\n", warmups[g_settings.warmup >= 0 && g_settings.warmup < WARMUP_COUNT ? g_settings.warmup : 0]);
    fprintf(f, "mute_unfocused = %d\n", g_settings.mute_unfocused);
    fprintf(f, "discord_presence = %d\n", g_settings.discord_presence);
    fprintf(f, "check_updates = %d\n", g_settings.check_updates);
    fprintf(f, "config_at_start = %d\n", g_settings.config_at_start);
    fprintf(f, "mouse_camera = %d\n", g_settings.mouse_camera);
    fprintf(f, "mouse_menus = %d\n", g_settings.mouse_menus);
    fprintf(f, "mouse_sensitivity = %d\n", g_settings.mouse_sensitivity);
    fprintf(f, "mouse_sensitivity_y = %d\n", g_settings.mouse_sensitivity_y);
    fprintf(f, "mouse_invert_x = %d\n", g_settings.mouse_invert_x);
    fprintf(f, "mouse_invert_y = %d\n", g_settings.mouse_invert_y);
    fprintf(f, "stick_camera_x = %d\n", g_settings.stick_camera_x);
    fprintf(f, "stick_camera_y = %d\n", g_settings.stick_camera_y);
    fprintf(f, "stick_camera_invert_x = %d\n", g_settings.stick_camera_invert_x);
    fprintf(f, "stick_camera_invert_y = %d\n", g_settings.stick_camera_invert_y);
    fprintf(f, "button_prompts = %s\n", g_settings.prompts == PROMPTS_CONTROLLER ? "controller" :
                                        g_settings.prompts == PROMPTS_KEYBOARD ? "keyboard" :
                                        g_settings.prompts == PROMPTS_SWITCH ? "switch" :
                                        g_settings.prompts == PROMPTS_XBOX ? "xbox" :
                                        g_settings.prompts == PROMPTS_PLAYSTATION ? "playstation" : "auto");
    if (g_settings.res_mode == RES_DYNAMIC) fprintf(f, "resolution = dynamic\n");
    else if (g_settings.res_mode == RES_Q75) fprintf(f, "resolution = 75%%\n");
    else if (g_settings.res_mode == RES_Q25) fprintf(f, "resolution = 25%%\n");
    else if (g_settings.res_mode == RES_Q50) fprintf(f, "resolution = 50%%\n");
    else if (g_settings.res_mode == RES_1440) fprintf(f, "resolution = 1440p\n");
    else if (g_settings.res_mode == RES_DISPLAY) fprintf(f, "resolution = 100%%\n");
    else if (g_settings.res_mode >= 1 && g_settings.res_mode <= 8) fprintf(f, "resolution = %dx\n", g_settings.res_mode);
    else fprintf(f, "resolution = match\n");
    fprintf(f, "language = %s\n", g_language_codes[g_settings.language > LANG_AUTO && g_settings.language < N_LANGUAGES
                                                     ? g_settings.language : LANG_AUTO]);
    fprintf(f, "menu_language = %s\n", g_settings.menu_language > 0 && g_settings.menu_language < N_LANGUAGES ? g_language_codes[g_settings.menu_language] : "auto");
    for (int i = 0; i < g_nmod_off; i++) fprintf(f, "mod_off = %s\n", g_mod_off[i]);
    bind_settings_save(f);
    fclose(f);
}

// Low memory: chosen, or Auto on a PC with less than 12 GB of RAM (an 8 GB laptop - its integrated GPU's memory is that
// RAM too). ODYSSEY_LOW_MEMORY=1 / 0 decides for tests.
bool settings_low_memory(void) {
    const char *e = getenv("ODYSSEY_LOW_MEMORY");
    if (e && *e) return atoi(e) != 0;
    if (g_settings.memory != MEMORY_AUTO) return g_settings.memory == MEMORY_LOW;
    static int low = -1;
    if (low < 0) {
        extern uint64_t plat_total_ram(void);
        uint64_t ram = plat_total_ram();
        low = ram && ram < (12ull << 30);
        LOG("[settings] %llu MB of RAM: %s memory use\n", (unsigned long long)(ram >> 20), low ? "low" : "normal");
    }
    return low;
}

volatile int g_display_hz;
// Match display (FPS_AUTO): the highest rate up to 120 the display's refresh is a whole multiple of - every frame on
// screen equally long, at a rate PCs hold (144 Hz -> 72, 240 -> 120, 180 -> 90, 120 -> 120, 60 -> 60); none: the
// refresh itself when it is one of the rates (165), else 60. Measured 2026-11-16, RTX 4090, 144 Hz, borderless: 60 FPS
// alternates 2 and 3 refreshes (p99 19 ms); 72: 2 refreshes each (p99 16 ms, 0-2 held per 10 s); 144: busy stretches
// at ~113 FPS (235 held per 10 s).
int settings_fps_hz(int frame_rate) {
    static const int hz[FPS_AUTO] = {60, 90, 120, 72, 100, 144, 165, 180, 240};
    if (frame_rate == FPS_AUTO) {
        int d = g_display_hz, best = 0;
        for (int i = 0; i < FPS_AUTO; i++)
            if (d > 0 && hz[i] <= 120 && d % hz[i] == 0 && hz[i] > best) best = hz[i];
        for (int i = 0; !best && i < FPS_AUTO; i++) if (hz[i] == d) best = d;
        return best ? best : 60;
    }
    return frame_rate >= 0 && frame_rate < FPS_AUTO ? hz[frame_rate] : 60;
}
int settings_game_hz(void) { return settings_fps_hz(g_settings.frame_rate); }
