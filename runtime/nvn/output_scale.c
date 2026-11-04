// Output resolution, shared by the Metal and Vulkan backends (each calls output_scale() once per presented frame).
// Render targets the game sizes from its screen (GAME_W x GAME_H: output_scale_texture) are allocated at (sx, sy)
// times their size; the image is then drawn into (vx, vy, vw, vh) of the window. Video tab "Internal resolution"
// (g_settings.res_mode), in lines of the 1080p output:
//   Match display  render at the window's own pixel size (default), at least the game's own 1600 x 900
//   1x .. 4x       N times 1080p: 1x = 1920 x 1080, 2x = 4K (wide windows keep their shape: 32:9 at 2x =
//                  7680 x 2160); capped to the GPU's 16384-pixel texture limit
//   .75x / .50x / .25x  of Match display (scaled up to the window - with MetalFX on macOS when chosen)
//   Dynamic        Match display, lowered in steps while the GPU can't hold 60 fps and raised again when it can
//                  (each change reallocates the render targets: a short hitch, so it changes rarely, with hysteresis)
// Test overrides: ODYSSEY_SCALE=<f> (render height = 1080 x f), ODYSSEY_WIDE_TEST=<f> (forced shape).
#include <math.h>
#include <stdlib.h>

#include "../hle.h"
#include "../settings.h"
#include "nvn.h"
#include "output_scale.h"

#define MAX_TEX 16384.0

// Odyssey's screen-sized targets: 1600x900 (scene, G-buffer, depth, the layout target, the display images it presents)
// and the chain of halves down to 12x7 (floor: 200x112, 100x56 ... - bloom, depth of field), 1280x720 and 320x180 -
// and the same at the lower sizes of its own dynamic resolution (1440x810 ... 960x540: it recreates them). All 2D (or
// 2D array) images of the screen's shape; NVN flags don't tell render targets apart (the layout target has none). Not:
// block-compressed textures (layout pictures), images for compute stores, video decoding or linear CPU access (NVN
// flags IMAGE, VIDEO_DECODE, LINEAR: they'd be written at guest coordinates), shadow maps and other squares, cube
// maps, multisampled targets. The CPU can still fill one (a video frame): textures.c scales that in.
// ODYSSEY_NO_SCALE=1: none (native size).
bool output_scale_texture(const NvnTexture *t) {
    static int off = -1;
    if (off < 0) off = getenv("ODYSSEY_NO_SCALE") != NULL;
    if (off || (t->target != 1 && t->target != 4) || t->samples > 1 || (t->flags & (2 | 4 | 0x10))) return false;
    if (!t->layout.fmt || t->layout.fmt->bw != 1 || t->width < 8 || t->height < 4) return false;
    float a = (float)t->width / (float)t->height;
    return a > 1.70f && a < 1.82f;  // 16:9, within the rounding of the small halves (12x7: 1.71)
}

// Dynamic resolution: a factor on the Match display scale, 0.5 .. 1 in 0.05 steps.
static double g_dyn = 1.0;
static double g_dyn_gpu_sum, g_dyn_dt_sum, g_last_t;
static int g_dyn_n, g_dyn_gpu_n, g_dyn_good;
static double g_dyn_cool_until, g_dyn_probe_wait = 5.0, g_dyn_probe_at;
static double g_dyn_last_raise = -100;

static void dyn_update(double now, double gpu_ms) {
    double dt = g_last_t > 0 ? (now - g_last_t) * 1000.0 : 16.7;
    g_last_t = now;
    if (dt > 250) return;  // (loading, menus, a stall: not the GPU's steady state)
    g_dyn_dt_sum += dt;
    g_dyn_n++;
    if (gpu_ms >= 0) { g_dyn_gpu_sum += gpu_ms; g_dyn_gpu_n++; }
    if (g_dyn_n < 30) return;  // decide every ~half second
    double avg_dt = g_dyn_dt_sum / g_dyn_n, avg_gpu = g_dyn_gpu_n ? g_dyn_gpu_sum / g_dyn_gpu_n : -1;
    g_dyn_dt_sum = g_dyn_gpu_sum = 0;
    g_dyn_n = g_dyn_gpu_n = 0;
    if (now < g_dyn_cool_until) return;
    double prev = g_dyn;
    bool slow = avg_dt > 17.4 || (avg_gpu >= 0 && avg_gpu > 14.5);
    if (slow) {
        g_dyn = fmax(0.5, g_dyn - (avg_dt > 22 ? 0.1 : 0.05));
        g_dyn_good = 0;
        if (now - g_dyn_last_raise < 3.0) g_dyn_probe_wait = fmin(g_dyn_probe_wait * 2, 60.0);  // raised too eagerly
    } else if (g_dyn < 1.0) {
        g_dyn_good++;
        // with GPU timing: raise when there is clear headroom; without (Vulkan): probe upwards now and then
        bool room = avg_gpu >= 0 ? avg_gpu < 10.5 && g_dyn_good >= 4 : now >= g_dyn_probe_at && g_dyn_good >= 2 * (int)g_dyn_probe_wait;
        if (room) { g_dyn = fmin(1.0, g_dyn + 0.05); g_dyn_good = 0; g_dyn_last_raise = now; g_dyn_probe_at = now + g_dyn_probe_wait; }
    }
    g_dyn = round(g_dyn * 20) / 20;
    if (g_dyn != prev) {
        g_dyn_cool_until = now + 1.5;
        LOG("[dynres] %.2f -> %.2f (frame %.1f ms, gpu %.1f ms)\n", prev, g_dyn, avg_dt, avg_gpu);
    }
}

double output_scale_dynamic_factor(void) { return g_dyn; }

void output_scale(double dw, double dh, bool reshape_allowed, double now, double gpu_ms, OutScale *o) {
    double fit = fmin(dw / 1920.0, dh / 1080.0);
    static double forced = -1, wide_test = -1;
    if (forced < 0) forced = getenv("ODYSSEY_SCALE") ? atof(getenv("ODYSSEY_SCALE")) : 0;
    if (wide_test < 0) wide_test = getenv("ODYSSEY_WIDE_TEST") ? atof(getenv("ODYSSEY_WIDE_TEST")) : 0;
    double shape = wide_test > 0 ? wide_test : (dw / dh) / (16.0 / 9.0);
    bool reshape = reshape_allowed && fabs(shape - 1.0) > 0.005;
    if (!reshape) shape = 1.0;
    // render height in units of the Switch's 1080 lines
    double h;
    int mode = g_settings.res_mode;
    if (forced > 0) h = forced;
    else if (mode >= 1 && mode <= 8) h = mode;
    else {
        h = reshape ? dh / 1080.0 : fit;  // Match display
        // ...but never below the game's own GAME_H lines: a smaller window (1280 x 720 at 100% display scaling) gets
        // the Switch's detail, filtered down (the shares below and Dynamic still go lower)
        if (mode == RES_MATCH || mode == RES_DYNAMIC) h = fmax(h, GAME_H / 1080.0);
        if (mode == RES_DYNAMIC) { dyn_update(now, gpu_ms); h *= g_dyn; }
        else if (mode == RES_Q75) h *= 0.75;
        else if (mode == RES_Q25) h *= 0.25;
        else if (mode == RES_Q50) h *= 0.5;
    }
    h = fmax(h, 0.1);  // (.25x of a small window)
    // the targets' scale: h lines of 1080 from the game's GAME_H
    double sx = h * shape * (1080.0 / GAME_H), sy = h * (1080.0 / GAME_H);
    double cap = fmin(MAX_TEX / (GAME_W * sx), MAX_TEX / (GAME_H * sy));
    if (cap < 1.0) { sx *= cap; sy *= cap; }
    o->sx = sx; o->sy = sy; o->shape = shape;
    // where the image goes in the window: fills it when reshaped, else a fitted 16:9 box
    if (reshape) { o->vw = dw; o->vh = dh; }
    else { o->vw = 1920.0 * fit; o->vh = 1080.0 * fit; }
    if (wide_test > 0) {
        double f2 = fmin(dw / (1920.0 * wide_test), dh / 1080.0);
        o->vw = 1920.0 * wide_test * f2; o->vh = 1080.0 * f2;
    }
    o->vx = (dw - o->vw) / 2; o->vy = (dh - o->vh) / 2;
}
