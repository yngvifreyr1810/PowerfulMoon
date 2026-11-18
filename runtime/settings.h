// User settings (settings.ini in the data root), edited from the in-game menu.
#pragma once
#include <stdbool.h>

enum { RES_MATCH = 0, /* 1..8 = 1x..8x */ RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12, RES_1440 = 13,
       RES_DISPLAY = 14 /* 100%: the window's own pixels (Steam Deck: 1280 x 800), no floor at the Switch's 900 lines */ };
enum { UPSCALE_BILINEAR = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* Windows: AMD FSR 1 */ };
#ifdef __APPLE__
#define UPSCALE_SHARP UPSCALE_METALFX  // each OS's non-bilinear upscaler (the default)
#else
#define UPSCALE_SHARP UPSCALE_FSR1
#endif
enum { AA_OFF = 0, AA_SMAA = 1, AA_FXAA = 2, AA_COUNT };
enum { OVERLAY_TOP_RIGHT, OVERLAY_TOP_LEFT, OVERLAY_BOTTOM_RIGHT, OVERLAY_BOTTOM_LEFT, OVERLAY_CORNER_COUNT };
enum { DLSS_OFF, DLSS_DLAA, DLSS_QUALITY, DLSS_BALANCED, DLSS_PERFORMANCE, DLSS_MODE_COUNT };
enum { DLSS_PRESET_DEFAULT, DLSS_PRESET_K, DLSS_PRESET_L, DLSS_PRESET_M, DLSS_PRESET_COUNT };
// (TEMPORAL_FSR2: AMD FSR 2.2 run by our own code - runtime/nvn/vulkan/fsr2.c; it shares FSR 3.1's mode and sharpness)
enum { TEMPORAL_DLSS, TEMPORAL_FSR3, TEMPORAL_FSR4, TEMPORAL_XESS, TEMPORAL_FSR2, TEMPORAL_BACKEND_COUNT };
enum { WINDOWED, BORDERLESS, EXCLUSIVE_FULLSCREEN, WINDOW_MODE_COUNT };
enum { DISPLAY_AUTO, DISPLAY_1080, DISPLAY_1440, DISPLAY_2160, DISPLAY_MODE_COUNT };
// VSync (Vulkan present modes, named as in Savage Wind Recomp - owner 2026-11-16): ON = FIFO (classic vsync: each frame
// waits for its refresh), FAST = mailbox (no tearing, the newest frame at each refresh, the game paces itself),
// ADAPTIVE = FIFO relaxed (On, but a late frame goes out at once - tearing then), OFF = immediate (tearing, the lowest
// latency; the default). The numbers are the settings file's older order (settings.ini "vsync": on = mailbox, strict
// = FIFO); the file now says "vsync_mode" (settings.c).
enum { VSYNC_FAST, VSYNC_OFF, VSYNC_ON, VSYNC_ADAPTIVE, VSYNC_COUNT };
static inline int settings_display_height(int mode) {
    const int heights[DISPLAY_MODE_COUNT] = {0, 1080, 1440, 2160};
    return heights[mode];
}
// Anisotropic filtering of surface textures: ANISO_GAME keeps the level the game asks for, else 2x..16x
enum { ANISO_GAME = 0 };
// Frame rate: FPS_90 / FPS_120 draw 90 / 120 times a second with gameplay at its 60 (runtime/hle/odyssey_120.c); FPS_60
// as on the Switch
enum { FPS_60 = 0, FPS_90 = 1, FPS_120 = 2, FPS_72, FPS_100, FPS_144, FPS_165, FPS_180, FPS_240, FPS_AUTO, FPS_COUNT };
int settings_fps_hz(int frame_rate);  // FPS_* -> frames per second (60, 90, 120, 72 ...; FPS_AUTO: from the display)
int settings_game_hz(void);           // the chosen frame rate's frames per second
extern volatile int g_display_hz;     // (the renderer) the display's refresh rate, whole Hz (0: not known)
// The image in a window of another shape: 16:9 with bars, or (Vulkan) filling a wider window with more of the scene
// at the sides (runtime/hle/odyssey_wide.c)
// or a fixed shape on any screen (bars where the screen's differs, as in PC games' aspect ratio options)
enum { ASPECT_16_9 = 0, ASPECT_FILL = 1, ASPECT_16_10 = 2, ASPECT_21_9 = 3, ASPECT_32_9 = 4, ASPECT_4_3 = 5, ASPECT_COUNT };
static const char *const ASPECT_NAMES[ASPECT_COUNT] = {"16:9", "fill", "16:10", "21:9", "32:9", "4:3"};
// the shape of each fixed one / 16:9 (0: the window's - fill); 21:9 is 64:27 (2560x1080, 5120x2160)
static const double ASPECT_K[ASPECT_COUNT] = {1.0, 0.0, (16.0 / 10.0) / (16.0 / 9.0), (64.0 / 27.0) / (16.0 / 9.0), 2.0, (4.0 / 3.0) / (16.0 / 9.0)};
// Console mode: what the game is told it runs as - docked (TV: its 1600x900 rendering, docked effects) or handheld (its
// own handheld rendering, 1280x720 and lighter effects, as on a Switch Lite). Applies at the next start.
enum { CONSOLE_DOCKED = 0, CONSOLE_HANDHELD = 1 };
// Button prompts: Auto follows the device used last (keyboard / mouse or controller)
// Memory use: Normal builds the shaders of what a stage sets up ahead (fewer stutters, ~3 GB more with NVIDIA's driver),
// Low builds each at its first draw; Auto is Low on a PC with less than 12 GB of RAM
enum { MEMORY_AUTO = 0, MEMORY_NORMAL, MEMORY_LOW, MEMORY_COUNT };
bool settings_low_memory(void);
// (Controller: the connected controller's own - Xbox, PlayStation, or the game's; the others always those)
enum { PROMPTS_AUTO = 0, PROMPTS_CONTROLLER = 1, PROMPTS_KEYBOARD = 2, PROMPTS_SWITCH, PROMPTS_XBOX, PROMPTS_PLAYSTATION,
       PROMPTS_COUNT };
// Language preference (runtime/services/am.c GetDesiredLanguage, services/hid_set.c GetLanguageCode):
// the game picks its text language from it, as it would from the Switch's own system setting. LANG_AUTO follows the
// host locale ($LC_ALL / $LC_MESSAGES / $LANG); the rest are the codes of the languages the game has text for.
enum { LANG_AUTO = 0, LANG_PT_BR = 12, N_LANGUAGES = 13 };
// The menu's own language: Auto follows the game's (when this menu has a translation for it), or a fixed one
// (the same list as the game's: an index into g_language_codes / g_language_names, 0 = Auto; a language with no
// translation file in lang/ shows the menu in English)
enum { MENU_LANG_AUTO = 0, MENU_LANG_COUNT = N_LANGUAGES };
extern const char *const g_language_codes[N_LANGUAGES];  // Persistencia; pt-BR usa en-US no jogo.
extern const char *const g_language_names[N_LANGUAGES];  // for the menu ("Auto", "Japanese", ..., "Spanish")

enum { FG_OFF, FG_FSR, FG_DLSS, FG_COUNT };  // (frame_gen: AMD FSR - fsr3_sdk.c; NVIDIA DLSS - sl_glue.cpp)
enum { AUDIO_OUT_AUTO, AUDIO_OUT_STEREO, AUDIO_OUT_SURROUND, AUDIO_OUT_COUNT };  // (audio_output: Auto = 5.1 when the device has 6+ channels)

typedef struct {
    int window_mode, display_resolution;
    int display_w, display_h;  // the window / exclusive mode's exact size (0: auto - display_resolution's old choices map here)
    int refresh_rate;    // exclusive fullscreen's refresh rate in Hz (0: the desktop's)
    int vsync;           // VSYNC_*: how frames reach the screen
    int monitor;         // 0: current display; 1..N: SDL's current display enumeration
    int res_mode;        // RES_MATCH, 1..4 (N x the Switch's 1080p), RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    int upscaler;        // UPSCALE_*: how a lower internal resolution reaches the window (MetalFX: macOS, FSR 1: Windows)
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    int dlss_mode, dlss_preset;  // Windows NVIDIA modes and model hints, applied during play
    bool temporal_off;   // the menu's Upscale method: Off (the default) - no temporal upscaler, whatever the modes below
    int frame_gen;          // FG_*: frame generation (Windows; needs an Upscale method): a generated frame between each two
    int low_latency;        // 0 off, 1 on, 2 on + boost: NVIDIA Reflex (Streamline: sl_glue.cpp)
    int hud_edges;          // pictures wider than 16:9: 1 the HUD at the picture's edges (default), 0 in a centred 16:9 box
    int temporal_backend, fsr3_mode, fsr3_sharpness, fsr3_cas_sharpness;
    bool fsr3_cas;
    int fsr4_mode, fsr4_sharpness, fsr4_cas_sharpness;
    int xess_mode, xess_sharpness, xess_cas_sharpness;
    bool fsr4_cas, xess_cas;
    bool dlss_cas;
    int dlss_sharpness;   // FidelityFX CAS percent, 0..100; zero disables sharpening
    int dlss_rcas_sharpness; // reconstruction sharpening, independent of optional CAS
    int fsr1_sharpness;   // FSR 1's RCAS percent, 0..100; stored independently
    int aspect;          // ASPECT_*: the image in a window wider than 16:9
    int console_mode;    // CONSOLE_*: docked or handheld (the game's own rendering for each; next start)
    bool game_dynres;    // the game's own dynamic resolution (on: as on the Switch; off: always its full resolution)
    bool frame_interp;   // always false: 120 FPS interpolation was removed (2026-11-16); ODYSSEY_INTERP for tests (nvn/interp.c)
    bool deband;         // smooth gradients: the game's final picture in half floats, dithered into the window (Vulkan:
                         // nvn/vulkan/textures.c vk_deband)
    float volume;        // master volume, 1 = 100%
    bool reverb;         // the audio renderer's reverb effect
    int audio_output;    // AUDIO_OUT_*: what the game is told the TV has (services/audio.c audio_channels, #35)
    bool movie_audio;
    bool show_fps, performance_overlay;
    // The performance overlay (runtime/ui/menu.mm draw_performance): where, how big, how see-through, and its parts
    int overlay_corner;   // OVERLAY_*
    int overlay_size;     // 0 small, 1 medium, 2 large
    int overlay_opacity;  // percent of the background, 0..100
    bool overlay_gpu, overlay_cpu, overlay_ram, overlay_graph, overlay_hardware, overlay_render, overlay_position;
    bool motion;         // the controller's gyro / accelerometer as the Switch's motion sensor (runtime/hle/input.c)
    int motion_shake_pad;  // a controller button (PADB_*) that shakes the controller for the game while held (-1: none)
    bool motion_invert_x;  // the gyro's pitch (tilting forward / back) the other way round (#53)
    int motion_filter;  // 0: original, 1: moderate, 2: strong
    bool motion_invert_y;  // the gyro's yaw (turning left / right) the other way round
    float motion_gyro_bias[3];  // the gyro's offset at rest, Switch axes, turns a second (Calibrate gyro; 0: learned as played)
    int motion_shake_key;  // a key (SDL scancode) or mouse button (BIND_MOUSE_*) doing the same (0: none)
    bool rumble;         // the controller's vibration (the game's HD rumble: runtime/hle/input.c input_vibration)
    int rumble_strength;  // 25..200 (%): gain before clamping to the controller's output range
    int stick_deadzone;  // percent of a controller stick's travel that counts as rest (runtime/hle/input.c), 0: none
    bool mouse_camera;   // the mouse turns the camera (the right stick; the pointer is held in the window while playing)
    bool mouse_menus;    // in the game's menus, the mouse buttons are A / B (bound or not) and an unbound wheel the D-pad (hle/input.c, #64)
    int mouse_sensitivity;  // percent, 25..400 (runtime/hle/input.c mouse_stick)
    int mouse_sensitivity_y; // independent vertical speed; old mouse_sensitivity files initialize both axes
    bool mouse_invert_x;
    bool mouse_invert_y; // moving the mouse up turns the camera down
    int stick_camera_x, stick_camera_y; // percent, 25..400; the camera only, never Mario's movement or menus
    bool stick_camera_invert_x, stick_camera_invert_y;
    int prompts;         // PROMPTS_*: the game's button icons as key caps / a mouse, or the controller's (nvn/kbprompts.c)
    int language;        // LANG_AUTO or an index into g_language_codes: the game's text language
    int menu_language;   // MENU_LANG_*: this menu's own text (runtime/ui/i18n.mm); applies at the next start
    int aniso;           // ANISO_GAME or 2, 4, 8, 16: the most anisotropic filtering of filtered, mipmapped textures that
                         // tile (nvn/vulkan/textures.c vk_sampler; read at start)
    int frame_rate;      // FPS_*: the game's draws per second (gameplay stays at 60)
    int memory;          // MEMORY_*: how much memory the renderer may use to spare stutters (settings_low_memory)
    bool shader_preload; // a stage's shaders get their libraries when the stage sets them up, not at their first draw
                         // (nvn/vulkan/exec.c compile_program; more memory)
    bool shader_wait;    // a draw whose shader is still loading waits for it (a hitch) instead of being left out /
                         // its frame not being shown (exec.c builds_block)
    int texture_detail;  // 0 as the Switch, 1 high, 2 ultra: a sharper mip bias on surface textures (textures.c; at start)
    int record_thread;   // -1 Auto (on with an NVIDIA GPU; the default), 0 off, 1 on: a second thread makes the draws' Vulkan calls
                         // (nvn/vulkan/exec.c dq_*; read at start)
    int early_fences;    // 1 on (the default), 0 off: the game goes on once its frame is translated, not
                         // when the GPU has run it (nvn/objects.c nvnSyncWait; ODYSSEY_SYNC_EARLY overrides)
    int shadow_quality;  // 0 as the Switch, 1 high (2x), 2 ultra (4x): shadow map size (nvn/vulkan/textures.c shadow_factor)
    bool shader_known_only; // shaders set up ahead only when drawn before (any run): less CPU after a stage loads,
                         // more first-draw builds (exec.c renderer_program_hint; ODYSSEY_PREWARM=3). Off by default.
    bool skip_intro;     // no Joy-Con tips screen timer before the title (runtime/hle/odyssey_120.c; read at start)
    int warmup;          // WARMUP_*: how long the start replays known pipelines (nvn/vulkan/exec.c vk_pipeline_prewarm)
    bool mute_unfocused; // no sound while the window isn't in front (services/audio.c)
    bool discord_presence; // the kingdom and time played on the player's Discord profile (runtime/discord.c)
    bool config_at_start;  // the configuration page before the game at every start (runtime/ui/setup.mm config_run)
    bool check_updates;  // look for a newer release at start (runtime/updater.c)
} Settings;
// The pipeline warm-up at start: Auto picks Long on PCs with 16 GB of memory or more
enum { WARMUP_AUTO = 0, WARMUP_NORMAL = 1, WARMUP_LONG = 2, WARMUP_COUNT };
// The start's warm-up budget for this PC: milliseconds and megabytes of extra memory
void settings_warmup_budget(unsigned *ms, unsigned *mb);
// The window isn't in front (set by the menu's event handling)
extern volatile bool g_app_unfocused;

extern Settings g_settings;

static inline int *settings_temporal_mode(void) {
    switch (g_settings.temporal_backend) {
    case TEMPORAL_FSR3: case TEMPORAL_FSR2: return &g_settings.fsr3_mode;
    case TEMPORAL_FSR4: return &g_settings.fsr4_mode;
    case TEMPORAL_XESS: return &g_settings.xess_mode;
    default: return &g_settings.dlss_mode;
    }
}
static inline int *settings_temporal_sharpness(void) {
    switch (g_settings.temporal_backend) {
    case TEMPORAL_FSR3: case TEMPORAL_FSR2: return &g_settings.fsr3_sharpness;
    case TEMPORAL_FSR4: return &g_settings.fsr4_sharpness;
    case TEMPORAL_XESS: return &g_settings.xess_sharpness;
    default: return &g_settings.dlss_rcas_sharpness;
    }
}
static inline bool *settings_temporal_cas(void) {
    switch (g_settings.temporal_backend) {
    case TEMPORAL_FSR3: case TEMPORAL_FSR2: return &g_settings.fsr3_cas;
    case TEMPORAL_FSR4: return &g_settings.fsr4_cas;
    case TEMPORAL_XESS: return &g_settings.xess_cas;
    default: return &g_settings.dlss_cas;
    }
}
static inline int *settings_temporal_cas_sharpness(void) {
    switch (g_settings.temporal_backend) {
    case TEMPORAL_FSR3: case TEMPORAL_FSR2: return &g_settings.fsr3_cas_sharpness;
    case TEMPORAL_FSR4: return &g_settings.fsr4_cas_sharpness;
    case TEMPORAL_XESS: return &g_settings.xess_cas_sharpness;
    default: return &g_settings.dlss_sharpness;
    }
}

void settings_load(void);
const char *settings_current_file(void);
void settings_save(void);
// The console mode in effect this run (read once: a change applies at the next start; ODYSSEY_HANDHELD=1 / =0 overrides)
bool settings_handheld(void);
// The game's dynamic resolution this run (read once; ODYSSEY_GAME_DYNRES=0 / =1 overrides)
bool settings_game_dynres(void);
// The language code to serve the game ("es", "en-US", ...): the setting, or the host locale for LANG_AUTO
const char *settings_language(void);
// That code's name for the menu ("Spanish")
const char *settings_language_name(void);
// Mods switched off in the menu (runtime/mods.c): their folders, one "mod_off = <folder>" line each
bool settings_mod_off(const char *id);
void settings_set_mod_off(const char *id, bool off);
// The anisotropy the game started with (the menu's value applies at the next start): 0 = the game's own
int settings_aniso_at_start(void);
