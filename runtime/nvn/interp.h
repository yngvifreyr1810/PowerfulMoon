// Frame interpolation (runtime/nvn/interp.c): images between the game's own frames, made by running the frame's
// command lists again with the per-draw uniforms blended between the previous frame and this one.
// Video tab "Frame interpolation" (off by default); ODYSSEY_INTERP overrides it (tests).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { INTERP_OFF = 0, INTERP_MEASURE = 1, INTERP_ON = 2 };
// What's wanted now (the setting, or ODYSSEY_INTERP): the backend's present asks once per game frame.
int interp_wanted(void);

// The executor's hooks (renderer.m). g_interp_log: the frame being executed is logged (draws + uniform copies);
// g_interp_replaying: a replay runs (skip side effects, read the logged / blended uniforms).
extern bool g_interp_log, g_interp_replaying;
bool interp_list_submitted(uint64_t handle);  // true for the frame's first list (the backend saves its state then)
bool interp_draw(uint64_t key, bool depth_only);  // false: a replayed draw without its record - leave it out  // depth_only: no colour target (a shadow pass)
const void *interp_cbuf(int stage, int k, int slot, const void *src, uint64_t size, uint64_t used);  // used: bytes the shader reads
// The texture-pool entry a draw's texture i resolves to: logged with the frame (a replay takes the logged one - the
// pool is the game's memory and it rewrites entries while the replays run; an area's lighting texture swapped under
// an in-between showed it with the next area's ambient light for one image).
const void *interp_texentry(int stage, int i, const void *entry, uint32_t size);
// Storage a draw's shader reads straight from guest memory (gmem): logged with the frame so a replay can tell when the
// game has rewritten it since (a replay reads it live; Dusklight's presented frame reads only its own recording), and
// a dispatch's output ranges, which the GPU rewrites in order and need no check.
void interp_storage(uint64_t addr, uint64_t size);
void interp_dispatch_output(uint64_t addr, uint64_t size);
void interp_texture_destroyed(void);  // (render thread, in queue order) a texture went away during the frame

// Objects freed at once by the game's threads (programs, memory pools, samplers): a replay must not run while one goes,
// nor use a frame that one went from after its lists were run.
void interp_free_begin(void);
void interp_free_end(void);

// The game's present closes the logged frame and hands it to the backend's interpolation thread, which matches it
// against the frame before (interp_frame_begin: whether in-between images can be made - false: a cut, level change,
// warp, camera cut, too few matches), replays it and rotates (interp_frame_rotate). log_next: whether the next frame is
// logged (the setting can change at any frame). NULL: nothing logged. A frame not taken goes back with interp_frame_drop.
typedef struct InterpFrame InterpFrame;
InterpFrame *interp_frame_close(bool log_next, uint64_t frame_no);
void interp_frame_drop(InterpFrame *f);
bool interp_frame_begin(InterpFrame *f);
// The backend's executor state at the frame's first list (saved while logging), where its replay starts.
void interp_frame_state_save(const void *p, size_t n);
const void *interp_frame_state(size_t *n);
extern uint64_t g_interp_present_frame;  // (the backend: the frame number of the present being made, for the research hooks)
void interp_force_cut(void);              // the next frame is a cut (the backend saw a level change, a resize...)
// Before the in-between of a frame: false when there must be none - the game thread is already two frames past it (its
// parity-double-buffered memory is being rewritten under the replay) or the storage its draws read changed since.
bool interp_replays_allowed(uint64_t frame, uint64_t game_frame);
bool interp_prepare(float t);             // blends for an image at t (0 = previous frame, 1 = this one)
bool interp_replay_lock(void);            // before replays: false when an object the frame used was freed
void interp_replay_unlock(void);
uint32_t interp_handles(const uint64_t **out);
void interp_replay_begin(void);
void interp_replay_end(void);
void interp_frame_rotate(void);           // (interpolation thread) after a frame: it becomes the previous one
void interp_log_stats(void);  // (interpolation thread, per game frame) a line every ODYSSEY_INTERP_LOG seconds (default 2)
