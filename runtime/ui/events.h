#pragma once
#include <SDL3/SDL.h>

// Wait once, then drain a bounded batch. Waiting again after every event can
// starve window/input maintenance forever when a gyro sends samples every 5 ms.
static inline bool ui_next_event(SDL_Event *event, unsigned *count) {
    if (*count >= 128) return false;
    bool have = *count ? SDL_PollEvent(event) : SDL_WaitEventTimeout(event, 16);
    if (have) ++*count;
    return have;
}
