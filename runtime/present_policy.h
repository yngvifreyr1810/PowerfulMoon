// MIT. Nominal refresh policy; never changes the game simulation clock.
#pragma once
static inline float pm_interp_hz(float current,float desktop,float supported,int target) {
    if (target==120) return supported>=119.0f ? 120.0f : 0.0f;
    return current>desktop ? current : desktop;
}
