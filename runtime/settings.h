// User settings (powermoon.ini in the data root), edited from the in-game menu.
#pragma once
#include <stdbool.h>

enum { RES_MATCH = 0, /* 1..8 = 1x..8x */ RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12 };
enum { UPSCALE_BILINEAR = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* Windows: AMD FSR 1 */ };
#ifdef __APPLE__
#define UPSCALE_SHARP UPSCALE_METALFX  // each OS's non-bilinear upscaler (the default)
#else
#define UPSCALE_SHARP UPSCALE_FSR1
#endif
enum { AA_OFF = 0, AA_SMAA = 1 };
// The image in a window of another shape: 16:9 with bars, or (Vulkan) filling a wider window with more of the scene
// at the sides (runtime/hle/odyssey_wide.c)
enum { ASPECT_16_9 = 0, ASPECT_FILL = 1 };
// System language served to the game (runtime/services/am.c GetDesiredLanguage, services/hid_set.c GetLanguageCode):
// the game picks its text language from it, as it would from the Switch's own system setting. LANG_AUTO follows the
// host locale ($LC_ALL / $LC_MESSAGES / $LANG); the rest are the codes of the languages the game has text for.
enum { LANG_AUTO = 0, N_LANGUAGES = 12 };
extern const char *const g_language_codes[N_LANGUAGES];  // what the game is told: "ja", "en-US", "es", ...
extern const char *const g_language_names[N_LANGUAGES];  // for the menu ("Auto", "Japanese", ..., "Spanish")

typedef struct {
    bool fullscreen;
    bool check_updates; // notify about approved fork releases; never install automatically
    int res_mode;        // RES_MATCH, 1..4 (N x the Switch's 1080p), RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    int upscaler;        // UPSCALE_*: how a lower internal resolution reaches the window (MetalFX: macOS, FSR 1: Windows)
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    int aspect;          // ASPECT_*: the image in a window wider than 16:9
    int interpolation_output; // 0: nominal display refresh, 120: explicit VRR interpolation target
    int vsync; // 0: auto/mailbox, 1: FIFO, 2: immediate (host presentation only)
    bool frame_interp;   // images between the game's frames (Vulkan: runtime/nvn/interp.c)
    bool deband;         // smooth gradients: the game's final picture in half floats, dithered into the window (Vulkan:
                         // nvn/vulkan/textures.c vk_deband)
    float volume;        // master volume, 1 = 100%
    bool reverb;         // the audio renderer's reverb effect
    bool movie_audio;
    bool show_fps;
    int stick_deadzone;  // percent of a controller stick's travel that counts as rest (runtime/hle/input.c), 0: none
    int language;        // LANG_AUTO or an index into g_language_codes: the game's text language
    bool motion, rumble, hd_rumble;
    float rumble_strength; // 0..2, default 1
} Settings;

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
// The language code to serve the game ("es", "en-US", ...): the setting, or the host locale for LANG_AUTO
const char *settings_language(void);
// That code's name for the menu ("Spanish")
const char *settings_language_name(void);
// Mods switched off in the menu (runtime/mods.c): their folders, one "mod_off = <folder>" line each
bool settings_mod_off(const char *id);
void settings_set_mod_off(const char *id, bool off);
