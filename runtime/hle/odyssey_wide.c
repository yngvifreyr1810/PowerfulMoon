// Screen shape (Video tab "Aspect ratio": Fill window). In a window wider than 16:9 the game shows more of its scene at
// the sides (Hor+) and keeps its HUD's shape in the middle; in a narrower one (a Steam Deck's 16:10) its view keeps its
// height and shows a little less at the sides, and the HUD keeps its 16:9 shape, fitted to the width (a little shorter,
// centred). The recompiler multiplies the game's own screen values (tools/odyssey_patches.py WIDE_SITES): its camera's
// aspect (odyssey_wide_camera: the shape), and the layout width where it sizes the screen (the layouts' projection, the
// life gauge and Cappy's off-screen marker kept on screen, the 2D effects: odyssey_wide / odyssey_wide_drawinfo - the
// shape when wider, 1 when narrower). The renderer allocates the screen targets in the shape (nvn/output_scale.c,
// odyssey_wide_shape).
//
// The shape the window asks for (odyssey_wide_want, render thread) takes effect between game frames, at the camera's
// update (game thread), so everything the game computes in a frame agrees. The layouts' projections are made once per
// scene (al::initDrawInfoDefault): a change rescales them in place - nn::ui2d::DrawInfo's projection matrix (+0x10, 4x4
// column-major; a centred orthographic / perspective one): its x row (offsets 0, 16, 32, 48) scales with 1 / the layout
// width, its y row (offsets 4, 20, 36, 52) by the shape when narrower (the HUD made shorter to keep its shape).
#include <math.h>
#include <string.h>

#include "../hle.h"

static volatile float g_want = 1.0f;  // (render thread) the window's aspect / 16:9 when the image fills it, else 1
static float g_wide = 1.0f;           // (game thread) the shape in effect

static float layout_k(float shape) { return shape > 1.0f ? shape : 1.0f; }  // the layout width's factor
static float hud_y(float shape) { return shape < 1.0f ? shape : 1.0f; }     // the HUD's height factor

#define NDI 16
static struct {
    uint64_t di;      // nn::ui2d::DrawInfo made by al::initDrawInfoDefault
    bool known;       // its rows read (after the init finished)
    float x[4], y[4]; // its x and y rows as last seen / written
    float k;          // the shape it was made / last rescaled for
    float ky;         // the y factor applied to it so far (1: as the game made it)
} g_di[NDI];
static int g_ndi;

// (WIDE_SITES other than the camera's update) the layout width's factor: the shape when wider than 16:9, else 1
float odyssey_wide(void) { return layout_k(g_wide); }
// (renderer) the image's aspect / 16:9
float odyssey_wide_shape(void) { return g_wide; }

void odyssey_wide_want(float k) { g_want = k >= 0.5f && k < 8.0f ? k : 1.0f; }

static float *di_x(uint64_t di, int i) { return (float *)(uintptr_t)(di + 0x10 + 16 * i); }
static float *di_y(uint64_t di, int i) { return (float *)(uintptr_t)(di + 0x14 + 16 * i); }

// (WIDE_SITES: al::initDrawInfoDefault, x20 = the DrawInfo) a layouts' projection made for the shape in effect
float odyssey_wide_drawinfo(uint64_t di) {
    int i = 0;
    while (i < g_ndi && g_di[i].di != di) i++;
    if (i == g_ndi) {
        if (g_ndi < NDI) g_ndi++;
        else memmove(g_di, g_di + 1, (NDI - 1) * sizeof g_di[0]), i = NDI - 1;  // (the oldest goes)
    }
    g_di[i].di = di;
    g_di[i].known = false;
    g_di[i].k = g_wide;
    g_di[i].ky = 1.0f;
    return layout_k(g_wide);
}

// (WIDE_SITES: al::CameraPoseUpdater::update, before its projection) the window's shape takes effect
float odyssey_wide_camera(void) {
    for (int i = 0; i < g_ndi; i++)
        if (!g_di[i].known && g_di[i].k == g_wide) {  // (made since: its rows, and the HUD's height when narrower)
            float ky = hud_y(g_wide);
            for (int j = 0; j < 4; j++) {
                g_di[i].x[j] = *di_x(g_di[i].di, j);
                if (ky != 1.0f) *di_y(g_di[i].di, j) *= ky;
                g_di[i].y[j] = *di_y(g_di[i].di, j);
            }
            g_di[i].ky = ky;
            g_di[i].known = true;
        }
    float want = g_want;
    if (want == g_wide) return g_wide;
    LOG("[wide] %.3f -> %.3f\n", g_wide, want);
    for (int i = 0; i < g_ndi; i++) {
        // still the matrix it was (a finished scene's DrawInfo may be gone, its memory reused): else forgotten
        bool same = g_di[i].known && g_di[i].k == g_wide;
        for (int j = 0; same && j < 4; j++)
            same = !memcmp(di_x(g_di[i].di, j), &g_di[i].x[j], 4) && !memcmp(di_y(g_di[i].di, j), &g_di[i].y[j], 4);
        if (!same) { g_di[i--] = g_di[--g_ndi]; continue; }
        float fx = layout_k(g_wide) / layout_k(want), ky = hud_y(want), fy = ky / g_di[i].ky;
        for (int j = 0; j < 4; j++) {
            *di_x(g_di[i].di, j) = g_di[i].x[j] = g_di[i].x[j] * fx;
            *di_y(g_di[i].di, j) = g_di[i].y[j] = g_di[i].y[j] * fy;
        }
        g_di[i].k = want, g_di[i].ky = ky;
    }
    g_wide = want;
    return g_wide;
}
