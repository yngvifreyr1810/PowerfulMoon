// Sampling profiler of the game thread (runtime/sampler.c; Linux, ODYSSEY_SAMPLE_AT=<frame>).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The game thread, at each present (its frame number): starts / stops the sampling
void sampler_frame(uint64_t frame);
void sampler_render_thread(void);  // (the render thread, at its start: ODYSSEY_SAMPLE_THREAD=render)
void sampler_main_thread(void);
void sampler_flush(void);          // (the exit: a sampling still going is written out)

#ifdef __cplusplus
}
#endif
