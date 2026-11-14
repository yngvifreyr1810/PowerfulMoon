// The player's persistent settings: loaded from powermoon.ini at start, saved on each menu edit.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define SETTINGS_FILE (settings_file())

enum { RES_MATCH = 0, /* 1..8 = 1x..8x */ RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12, RES_1440 = 13 };
enum { UPSCALE_BILINEAR = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* Windows: AMD FSR 1 */ };
#ifdef __APPLE__
#define UPSCALE_SHARP UPSCALE_METALFX  // each OS's non-bilinear upscaler (the default)
#else
#define UPSCALE_SHARP UPSCALE_FSR1
#endif
enum { AA_OFF = 0, AA_SMAA = 1 };
enum { OVERLAY_TOP_RIGHT, OVERLAY_TOP_LEFT, OVERLAY_BOTTOM_RIGHT, OVERLAY_BOTTOM_LEFT, OVERLAY_CORNER_COUNT };
enum { DLSS_OFF, DLSS_DLAA, DLSS_QUALITY, DLSS_BALANCED, DLSS_PERFORMANCE, DLSS_MODE_COUNT };
enum { DLSS_PRESET_DEFAULT, DLSS_PRESET_K, DLSS_PRESET_L, DLSS_PRESET_M, DLSS_PRESET_COUNT };
enum { TEMPORAL_DLSS, TEMPORAL_FSR3, TEMPORAL_FSR4, TEMPORAL_XESS, TEMPORAL_BACKEND_COUNT };
enum { WINDOWED, BORDERLESS, EXCLUSIVE_FULLSCREEN, WINDOW_MODE_COUNT };
enum { DISPLAY_AUTO, DISPLAY_1080, DISPLAY_1440, DISPLAY_2160, DISPLAY_MODE_COUNT };
static inline int settings_display_height(int mode) {
    const int heights[DISPLAY_MODE_COUNT] = {0, 1080, 1440, 2160};
    return heights[mode];
}
// Anisotropic filtering of surface textures: ANISO_GAME keeps the level the game asks for, else 2x..16x
enum { ANISO_GAME = 0 };
// Frame rate: FPS_90 / FPS_120 draw 90 / 120 times a second with gameplay at its 60 (runtime/hle/odyssey_120.c); FPS_60
// as on the Switch
enum { FPS_60 = 0, FPS_90 = 1, FPS_120 = 2, FPS_COUNT };
// The image in a window of another shape: 16:9 with bars, or (Vulkan) filling a wider window with more of the scene
// at the sides (runtime/hle/odyssey_wide.c)
enum { ASPECT_16_9 = 0, ASPECT_FILL = 1 };
// Memory use: Normal builds the shaders of what a stage sets up ahead, Low builds each at its first draw
enum { MEMORY_AUTO = 0, MEMORY_NORMAL, MEMORY_LOW, MEMORY_COUNT };
bool settings_low_memory(void);
// Button prompts: Auto follows the device used last (keyboard / mouse or controller)
enum { PROMPTS_AUTO = 0, PROMPTS_CONTROLLER = 1, PROMPTS_KEYBOARD = 2, PROMPTS_SWITCH, PROMPTS_XBOX, PROMPTS_PLAYSTATION,
       PROMPTS_COUNT };
// System language served to the game
enum { LANG_AUTO = -1 };
#define N_LANGUAGES 13
extern const char *const g_language_codes[N_LANGUAGES];
extern const char *const g_language_names[N_LANGUAGES];

typedef struct {
    bool fullscreen;
    int window_mode, display_resolution;
    int monitor;         // 0: current display; 1..N: SDL's current display enumeration
    int res_mode;        // RES_MATCH, 1..4 (N x the Switch's 1080p), RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    int upscaler;        // UPSCALE_*: how a lower internal resolution reaches the window (MetalFX: macOS, FSR 1: Windows)
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    int dlss_mode, dlss_preset;  // Windows NVIDIA modes and model hints, applied during play
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
    int interpolation_output; // 0: nominal display refresh, 120: explicit VRR interpolation target
    int vsync;           // 0: auto/mailbox, 1: on, 2: off
    bool deband;         // smooth gradients (dithering of the tonemapper's output; vulkan/textures.c)
    bool frame_interp;   // in-between images with blended uniforms (runtime/nvn/interp.c)
    float volume;        // master volume (0.0 .. 1.0)
    bool reverb;         // reverberation effect in audio mixes
    bool movie_audio;    // audio playback during cutscenes
    bool show_fps;       // top-left corner FPS text
    bool performance_overlay; // HUD performance overlay
    int overlay_corner;   // OVERLAY_*
    int overlay_size;     // 0 small, 1 medium, 2 large
    int overlay_opacity;  // percent of the background, 0..100
    bool overlay_gpu, overlay_cpu, overlay_ram, overlay_graph, overlay_hardware, overlay_render, overlay_position;
    bool motion;         // the controller's gyro / accelerometer as the Switch's motion sensor (runtime/hle/input.c)
    bool rumble, hd_rumble; // controller vibration & Switch 2 HD rumble
    float rumble_strength; // 0..2, default 1
    int stick_deadzone;  // percent of a controller stick's travel that counts as rest (runtime/hle/input.c), 0: none
    bool mouse_camera;   // the mouse turns the camera (the right stick; the pointer is held in the window while playing)
    int mouse_sensitivity;  // percent, 25..400 (runtime/hle/input.c mouse_stick)
    bool mouse_invert_y; // moving the mouse up turns the camera down
    int prompts;         // PROMPTS_*: button icons
    int language;        // LANG_AUTO or an index into g_language_codes: the game's text language
    int aniso;           // ANISO_GAME or 2, 4, 8, 16
    int frame_rate;      // FPS_*
    int memory;          // MEMORY_*
    bool shader_preload;
    bool shader_wait;
    bool skip_intro;
    int warmup;          // WARMUP_*
    bool mute_unfocused;
    bool check_updates;  // look for newer approved PowerfulMoon release
} Settings;

enum { WARMUP_AUTO = 0, WARMUP_NORMAL = 1, WARMUP_LONG = 2, WARMUP_COUNT };
void settings_warmup_budget(unsigned *ms, unsigned *mb);
extern volatile bool g_app_unfocused;

extern Settings g_settings;

static inline int *settings_temporal_mode(void) {
    switch (g_settings.temporal_backend) {
    case TEMPORAL_FSR3: return &g_settings.fsr3_mode;
    case TEMPORAL_FSR4: return &g_settings.fsr4_mode;
    case TEMPORAL_XESS: return &g_settings.xess_mode;
    default: return &g_settings.dlss_mode;
    }
}
static inline int *settings_temporal_sharpness(void) {
    switch (g_settings.temporal_backend) {
    case TEMPORAL_FSR3: return &g_settings.fsr3_sharpness;
    case TEMPORAL_FSR4: return &g_settings.fsr4_sharpness;
    case TEMPORAL_XESS: return &g_settings.xess_sharpness;
    default: return &g_settings.dlss_rcas_sharpness;
    }
}
static inline bool *settings_temporal_cas(void) {
    switch (g_settings.temporal_backend) {
    case TEMPORAL_FSR3: return &g_settings.fsr3_cas;
    case TEMPORAL_FSR4: return &g_settings.fsr4_cas;
    case TEMPORAL_XESS: return &g_settings.xess_cas;
    default: return &g_settings.dlss_cas;
    }
}
static inline int *settings_temporal_cas_sharpness(void) {
    switch (g_settings.temporal_backend) {
    case TEMPORAL_FSR3: return &g_settings.fsr3_cas_sharpness;
    case TEMPORAL_FSR4: return &g_settings.fsr4_cas_sharpness;
    case TEMPORAL_XESS: return &g_settings.xess_cas_sharpness;
    default: return &g_settings.dlss_sharpness;
    }
}

void settings_load(void);
const char *settings_current_file(void);
void settings_save(void);
void settings_battery_saver(void);
const char *settings_language(void);
const char *settings_language_name(void);
int settings_aniso_at_start(void);
