// The render thread (runtime/nvn/rqueue.c): the game's command lists, fences and presents, in order, off its thread.
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { RQ_EXEC = 1, RQ_FENCE, RQ_FRAME, RQ_PRESENT, RQ_DLSS_CAMERA, RQ_ODYSSEY_POSE, RQ_CPU_TEXEL, RQ_PAINT_WRITE, RQ_EXTRA_FRAME };
typedef struct { uint64_t addr, size; int heap; } RqPaintWrite;  // (heap: malloc'd by the sender, freed once run)
typedef struct {
    uint64_t addr;
    uint32_t x, y, component, components;
    uint32_t value;
    bool valid;
    uint8_t component_bytes; // 0: F16 components (2 bytes); 4: one packed RGBA8 element
    void *snapshot; // optional caller-owned, tightly packed staging destination (never guest memory)
    uint64_t snapshot_capacity, guest_capacity, guest_pitch;
    uint32_t snapshot_width, snapshot_height;
    bool refresh_snapshot; // CPU filter starts after GPU drawing within the same present
    uint64_t have_gen;     // in: the copy the caller's rows already hold (0: none); out (gen): the copy returned
    uint64_t gen;
    bool unchanged;        // out: gen == have_gen - the snapshot was not filled, the caller's rows are it
    uint32_t dbg_path, dbg_wgen, dbg_wseq;  // (tests) how it was answered: 1 kept, 2 the copy at the write, 3 a copy now; the map's write count; the kept copy's
    uint64_t dbg_frame;
} RqCpuTexel;
bool rq_enabled(void);                            // (ODYSSEY_RENDER_THREAD=0: no; macOS: no)
bool rq_on_thread(void);                          // the caller is the render thread
uint64_t rq_push(int type, void *p, uint64_t a);  // queued; returns its ticket
void rq_wait(uint64_t ticket);                    // until that item is done
// A paint map ask (RQ_CPU_TEXEL) run as soon as the render thread has done the last queued item that may write a
// paint map, ahead of what is queued behind it (rqueue.c); waits for it. Returns the last ticket queued at the ask.
uint64_t rq_ask(int type, void *p);
void rq_drain(void);                              // until everything queued so far is done
void rq_present_throttle(void);                   // until no present is queued: one frame ahead at most
uint64_t rq_last_ticket(void);                    // the newest item's ticket
bool rq_is_done(uint64_t ticket);                 // that item is done (no waiting)
uint64_t rq_done_ticket(void);                    // the last ticket the render thread finished
