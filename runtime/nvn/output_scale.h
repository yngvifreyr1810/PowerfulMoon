// Output resolution (runtime/nvn/output_scale.c), shared by the Metal and Vulkan backends.
#pragma once
#include <stdbool.h>

// The game's own screen size: Odyssey renders its scene at 1600x900 (docked; the Switch scales it to 1080p), so its
// screen-sized targets are 1600x900 and fractions of it.
#define GAME_W 1600.0
#define GAME_H 900.0

struct NvnTexture;
// A render target sized from the game's screen (allocated at (sx, sy) times its size; both backends).
bool output_scale_texture(const struct NvnTexture *t);

typedef struct {
    double sx, sy;             // render-target scale (relative to the game's GAME_W x GAME_H sizes)
    double shape;              // the image's aspect / 16:9 (the game's wide screen factor), 1: 16:9
    double vx, vy, vw, vh;     // where the image is drawn in the window (pixels)
} OutScale;

// dw x dh: the window's drawable size in pixels; want: the image's aspect / 16:9 (1: 16:9; runtime/hle/odyssey_wide.c);
// now: seconds; gpu_ms: GPU time of the last frame (< 0: unknown)
void output_scale(double dw, double dh, double want, double now, double gpu_ms, OutScale *o);
double output_scale_dynamic_factor(void);
