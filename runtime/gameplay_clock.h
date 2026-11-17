#pragma once
#include <stdbool.h>
#include <stdint.h>

// Bounded 60 Hz update debt for a render loop that still performs one update
// minimum per iteration. Only additional updates are scheduled here.
typedef struct { uint64_t last_ns; int64_t debt; } GameplayClock;
static inline unsigned gameplay_clock_steps(GameplayClock *c, uint64_t now, bool active) {
    uint64_t delta = now - c->last_ns;
    if (!active || !c->last_ns || now < c->last_ns || delta >= 100000000ull) {
        c->last_ns = now; c->debt = 0; return 1;
    }
    c->last_ns = now;
    c->debt += (int64_t)delta * 60;
    unsigned steps = c->debt >= 1000000000ll ? (unsigned)(c->debt / 1000000000ll) : 1;
    if (steps > 3) steps = 3;
    c->debt -= (int64_t)steps * 1000000000ll;
    if (c->debt > 2000000000ll) c->debt = 2000000000ll;
    if (c->debt < -1000000000ll) c->debt = -1000000000ll;
    return steps;
}
