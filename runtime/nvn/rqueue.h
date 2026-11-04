// The render thread (runtime/nvn/rqueue.c): the game's command lists, fences and presents, in order, off its thread.
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { RQ_EXEC = 1, RQ_FENCE, RQ_FRAME, RQ_PRESENT };
bool rq_enabled(void);                            // (ODYSSEY_RENDER_THREAD=0: no; macOS: no)
bool rq_on_thread(void);                          // the caller is the render thread
uint64_t rq_push(int type, void *p, uint64_t a);  // queued; returns its ticket
void rq_wait(uint64_t ticket);                    // until that item is done
void rq_drain(void);                              // until everything queued so far is done
void rq_present_throttle(void);                   // until no present is queued: one frame ahead at most
uint64_t rq_last_ticket(void);                    // the newest item's ticket
