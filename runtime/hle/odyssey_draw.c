// Draw distance (Graphics tab "Draw distance": Switch / High / Very high). The game hides and stops the actors (enemies,
// coins, moons, NPCs, many map parts) beyond a far clipping distance, switches models to their simpler level of detail
// by distance, and fades some far objects out (al::FarDistanceDitherAnimator). The recompiler multiplies those
// distances by odyssey_draw_distance() right where the game reads them (tools/odyssey_patches.py DRAW_SITES, 1.0.0 and
// 1.4.1):
//   - al::ClippingJudge::update: the far clipping distance (al::ClippingFarAreaObserver's, 7000 by default, a stage's
//     or a ClippingFarArea's own) handed to every view's al::FrustumRadar - the far limit of every actor's clipping;
//   - al::ModelLodCtrl::update: the camera distance the level of detail is chosen by (model, shadow and material
//     levels), divided by the factor: the detailed models reach that much farther;
//   - al::FarDistanceDitherAnimator::update: the distance its object fades out at.
// The camera's own far plane is left as it is. Read every game frame: a change applies at once.
// Not 1:1 with the Switch above 1: actors farther away keep moving (they are frozen while clipped on the Switch), and
// the CPU and GPU do more.
#include <stdlib.h>

#include "../hle.h"
#include "../settings.h"

float odyssey_draw_distance(void) {
    static float env = -1.0f;
    if (env < 0.0f) {
        const char *e = getenv("ODYSSEY_DRAW_DISTANCE");
        float v = e && *e ? (float)atof(e) : 0.0f;
        env = v >= 0.25f && v <= 64.0f ? v : 0.0f;
    }
    if (env > 0.0f) return env;
    static const float k[] = {1.0f, 2.0f, 4.0f};
    int q = g_settings.draw_distance;
    return k[q < 0 ? 0 : q > 2 ? 2 : q];
}
