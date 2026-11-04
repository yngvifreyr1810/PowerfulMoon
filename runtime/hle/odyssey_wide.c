// Wide screen (Video tab "Aspect ratio": Fill window): in a window wider than 16:9 the game shows more of its scene at
// the sides (Hor+) and keeps its HUD's shape in the middle. The recompiler multiplies the game's own screen values by
// odyssey_wide() (tools/odyssey_patches.py WIDE_SITES): its camera's aspect, and the layout width where it sizes the
// screen (the layouts' projection, the life gauge and Cappy's off-screen marker kept on screen, the 2D effects). The
// renderer allocates the screen targets that much wider (nvn/output_scale.c: the shape is odyssey_wide()).
//
// The factor the window asks for (odyssey_wide_want, render thread) takes effect between game frames, at the camera's
// update (game thread), so everything the game computes in a frame agrees. The layouts' projections are made once per
// scene (al::initDrawInfoDefault): a change rescales them in place - the x row of nn::ui2d::DrawInfo's projection
// matrix (+0x10, 4x4 column-major; a centred perspective: x scales with 1 / aspect).
#include <math.h>
#include <string.h>

#include "../hle.h"

static volatile float g_want = 1.0f;  // (render thread) the window's aspect / 16:9 when the image fills it, else 1
static float g_wide = 1.0f;           // (game thread) in effect

#define NDI 16
static struct {
    uint64_t di;      // nn::ui2d::DrawInfo made by al::initDrawInfoDefault
    bool known;       // its x row read (after the init finished)
    float x[4], k;    // the x row as last seen / written, and the factor it was made for
} g_di[NDI];
static int g_ndi;

float odyssey_wide(void) { return g_wide; }

void odyssey_wide_want(float k) { g_want = k >= 1.0f && k < 8.0f ? k : 1.0f; }

static float *di_row(uint64_t di, int i) { return (float *)(uintptr_t)(di + 0x10 + 16 * i); }

// (WIDE_SITES: al::initDrawInfoDefault, x20 = the DrawInfo) a layouts' projection made for the factor in effect
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
    return g_wide;
}

// (WIDE_SITES: al::CameraPoseUpdater::update, before its projection) the window's factor takes effect
float odyssey_wide_camera(void) {
    for (int i = 0; i < g_ndi; i++)
        if (!g_di[i].known && g_di[i].k == g_wide) {
            for (int j = 0; j < 4; j++) g_di[i].x[j] = *di_row(g_di[i].di, j);
            g_di[i].known = true;
        }
    float want = g_want;
    if (want == g_wide) return g_wide;
    LOG("[wide] %.3f -> %.3f\n", g_wide, want);
    for (int i = 0; i < g_ndi; i++) {
        // still the matrix it was (a finished scene's DrawInfo may be gone, its memory reused): else forgotten
        bool same = g_di[i].known && g_di[i].k == g_wide;
        for (int j = 0; same && j < 4; j++) same = !memcmp(di_row(g_di[i].di, j), &g_di[i].x[j], 4);
        if (!same) { g_di[i--] = g_di[--g_ndi]; continue; }
        for (int j = 0; j < 4; j++) *di_row(g_di[i].di, j) = g_di[i].x[j] = g_di[i].x[j] * g_wide / want;
        g_di[i].k = want;
    }
    g_wide = want;
    return g_wide;
}
