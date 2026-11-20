// The render thread: the game's command lists are translated to the GPU API here, while the game's thread goes on
// with its next frame - as the Switch's GPU works through a queue while the CPU runs ahead, and as emulators run their
// GPU on a thread of its own. Translated on the submitting thread, the lists cost the game ~4.6 of its ~5.4 ms a frame
// in the Sand Kingdom (Ryzen 9700X, ~1700 draws): on 4-core handhelds (Steam Deck) the game fell to 30-35 fps.
//
// In order on the thread: lists (nvnQueueSubmitCommands), the queue's fences (their GPU submission is known once the
// lists before them are translated), the frame's end and its present. The game waits for the thread where it needs its
// results: a fence wait, a queue finish, freeing a program / pool / command buffer a queued list may use, and one frame
// ahead at most (the present). ODYSSEY_RENDER_THREAD=0: lists run on the submitting thread, as before (macOS always).
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "../hle.h"
#include "../kernel.h"
#include "../odyssey_pose.h"
#include "../platform.h"
#include "nvn.h"
#include "rqueue.h"
#ifndef __APPLE__  // (the temporal upscalers: every Vulkan build)
#include "vulkan/dlss_probe.h"
#endif

uint64_t renderer_fence_seq(void);  // (the backend: the GPU submission being recorded)

#define RQ_N 8192
typedef struct { int type; void *p; uint64_t a, ticket; } RqItem;
static RqItem q[RQ_N];
static uint64_t q_head, q_tail;            // (q_mx) items [q_head, q_tail) queued
static uint64_t q_ticket;                  // (q_mx) the last ticket handed out
static _Atomic uint64_t q_done;            // the last ticket the thread finished
static _Atomic int q_presents;             // presents queued and not yet done
static pthread_mutex_t q_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER, q_done_cv = PTHREAD_COND_INITIALIZER;
static __thread bool on_thread;

bool rq_enabled(void) {
#ifdef __APPLE__
    return false;  // (the Metal backend presents from its own handlers: not with this thread)
#else
    static int on = -1;
    if (on < 0) { const char *e = getenv("ODYSSEY_RENDER_THREAD"); on = !e || atoi(e) != 0; }
    return on;
#endif
}
bool rq_on_thread(void) { return on_thread; }

#ifdef __linux__  // (thread names: what a profiler or /proc/<pid>/task/*/comm shows)
#include <sys/prctl.h>
#define RT_THREAD_NAME(...) do { char tn_[16]; snprintf(tn_, sizeof tn_, __VA_ARGS__); prctl(PR_SET_NAME, tn_); } while (0)
#else
#define RT_THREAD_NAME(...) do { } while (0)
#endif
// A paint map ask ahead of the queue (rq_ask): one at a time.
static struct { bool pending, scanned; int type; void *p; uint64_t limit, after, seq; } prio;
// The ask now waiting (its number; 0: none, or one that cannot be answered inside the list being run): exec.c looks
// at it between the commands of a list (paint_ask_poll).
static _Atomic uint64_t ask_waiting;
static uint64_t ask_seq, ask_mid;
uint64_t rq_ask_waiting(void) { return atomic_load_explicit(&ask_waiting, memory_order_relaxed); }
static uint64_t prio_served, prio_early;
static _Atomic uint64_t prio_window;
static _Atomic uint64_t ask_wait_ns;
static uint64_t ask_path_n[4], ask_path_ns[4];  // (tests) by how it was answered: 1 kept, 2 the copy at the write, 3 a copy now
static void ask_note(const RqCpuTexel *q, uint64_t ns) { unsigned k = q->dbg_path < 4 ? q->dbg_path : 0; ask_path_n[k]++, ask_path_ns[k] += ns; }
// ODYSSEY_LOG_PAINT_SYNC=1: how the asks were answered and how long the game thread waited for one (q_mx held)
static void ask_stats_log(void) {
    static int lg = -1;
    static uint64_t served0, mid0;
    if (lg < 0) lg = getenv("ODYSSEY_LOG_PAINT_SYNC") != NULL;
    if (prio_served - served0 < 1000) return;
    if (lg) LOG("[paint-sync] %llu asks: %llu answered inside a list, %llu between lists; %.2f ms waited an ask" "\n",
                (unsigned long long)(prio_served - served0), (unsigned long long)(ask_mid - mid0),
                (unsigned long long)(prio_served - served0 - (ask_mid - mid0)), atomic_load(&ask_wait_ns) / 1e6 / (double)(prio_served - served0));
    if (lg) LOG("[paint-sync]   answers: other %llu (%.2f ms each), kept %llu (%.2f), the copy at the write %llu (%.2f), a copy now %llu (%.2f)\n",
                (unsigned long long)ask_path_n[0], ask_path_ns[0] / 1e6 / (ask_path_n[0] ? ask_path_n[0] : 1), (unsigned long long)ask_path_n[1], ask_path_ns[1] / 1e6 / (ask_path_n[1] ? ask_path_n[1] : 1),
                (unsigned long long)ask_path_n[2], ask_path_ns[2] / 1e6 / (ask_path_n[2] ? ask_path_n[2] : 1), (unsigned long long)ask_path_n[3], ask_path_ns[3] / 1e6 / (ask_path_n[3] ? ask_path_n[3] : 1));
    for (int i = 0; i < 4; i++) ask_path_n[i] = ask_path_ns[i] = 0;
    served0 = prio_served, mid0 = ask_mid;
    atomic_store(&ask_wait_ns, 0);
}
// (exec.c paint_ask_poll, the render thread inside a queued list) that ask can be answered inside this list as far as
// the queue goes: nothing else queued before it draws or uploads (only fences, frame marks, presents). Whether the rest
// of the list itself writes a paint map is exec.c's to know.
bool rq_ask_mid_begin(uint64_t seq) {
    pthread_mutex_lock(&q_mx);
    bool ok = prio.pending && prio.seq == seq && prio.type == RQ_CPU_TEXEL;
    for (uint64_t i = q_head + 1; ok && i != q_tail; i++) {
        const RqItem *x = &q[i % RQ_N];
        if (x->ticket > prio.limit) break;
        if (x->type != RQ_FENCE && x->type != RQ_FRAME && x->type != RQ_PRESENT && x->type != RQ_DLSS_CAMERA && x->type != RQ_ODYSSEY_POSE) ok = false;
    }
    void *p = prio.p;
    pthread_mutex_unlock(&q_mx);
    extern bool renderer_paint_known(uint64_t addr);
    if (ok) ok = renderer_paint_known(((const RqCpuTexel *)p)->addr);  // (a map's first ask: behind everything)
    if (!ok) { uint64_t s = seq; atomic_compare_exchange_strong(&ask_waiting, &s, 0); }  // (not this one: no more looks)
    return ok;
}
// ODYSSEY_PAINT_MID_VERIFY=1 (tests, at full speed): every answer given inside a list is asked again when that list
// is done - where the ask was answered before - and must be the same map / texel: nothing in the rest of the list
// may have written it.
typedef struct { RqCpuTexel q; void *bytes; uint64_t n; } MidRec;
static MidRec midv[1024];
static int midv_n, midv_on = -1;
static void midv_note(const RqCpuTexel *q) {
    if (midv_on < 0) midv_on = getenv("ODYSSEY_PAINT_MID_VERIFY") != NULL;
    if (!midv_on || midv_n >= 1024) return;
    MidRec *m = &midv[midv_n++];
    m->q = *q, m->bytes = NULL, m->n = (uint64_t)q->snapshot_width * 4 * q->snapshot_height;
    if (q->snapshot && q->valid && !q->unchanged && m->n && m->n <= q->snapshot_capacity && (m->bytes = malloc(m->n))) memcpy(m->bytes, q->snapshot, m->n);
}
static void midv_check(void) {  // (the render thread, the list done)
    static uint64_t same, diff;
    extern void renderer_cpu_texel(RqCpuTexel *);
    for (int i = 0; i < midv_n; i++) {
        MidRec *m = &midv[i];
        RqCpuTexel q2 = {.addr = m->q.addr, .x = m->q.x, .y = m->q.y, .component = m->q.component, .components = m->q.components,
                         .component_bytes = m->q.component_bytes, .guest_capacity = m->q.guest_capacity, .guest_pitch = m->q.guest_pitch};
        bool ok;
        if (m->q.snapshot) {
            q2.snapshot_capacity = m->q.snapshot_capacity, q2.snapshot = malloc(q2.snapshot_capacity), q2.have_gen = m->q.valid ? m->q.gen : 0;
            renderer_cpu_texel(&q2);
            ok = q2.valid == m->q.valid && (!q2.valid || q2.unchanged ||
                 (m->bytes && (uint64_t)q2.snapshot_width * 4 * q2.snapshot_height == m->n && !memcmp(m->bytes, q2.snapshot, m->n)));
            free(q2.snapshot);
        } else {
            renderer_cpu_texel(&q2);
            ok = q2.valid == m->q.valid && (!q2.valid || q2.value == m->q.value);
        }
        if (ok) same++;
        else if (diff++ < 16) LOG("[paint-verify] DIFFERENT (answered inside a list): %llx rows %d valid %d/%d gen %llu/%llu texel %08x/%08x" "\n",
                                  (unsigned long long)m->q.addr, m->q.snapshot != NULL, m->q.valid, q2.valid, (unsigned long long)m->q.gen, (unsigned long long)q2.gen, m->q.value, q2.value);
        if ((same + diff) % 5000 == 0)
            LOG("[paint-verify] answers inside a list: %llu the same when the list was done, %llu different" "\n", (unsigned long long)same, (unsigned long long)diff);
        free(m->bytes);
    }
    midv_n = 0;
}
void rq_ask_mid_serve(uint64_t seq) {
    pthread_mutex_lock(&q_mx);
    if (!prio.pending || prio.seq != seq) { pthread_mutex_unlock(&q_mx); return; }
    void *p = prio.p;
    pthread_mutex_unlock(&q_mx);
    { extern void renderer_cpu_texel(RqCpuTexel *); uint64_t t0 = plat_now_ns(); renderer_cpu_texel(p); ask_note(p, plat_now_ns() - t0); }
    midv_note(p);
    pthread_mutex_lock(&q_mx);
    prio.pending = false;
    atomic_store(&ask_waiting, 0);
    prio_served++, prio_early++, ask_mid++;
    atomic_store(&prio_window, prio.limit);  // (tests: writes to a paint map before this is done were missed)
    ask_stats_log();
    pthread_cond_broadcast(&q_done_cv);
    pthread_mutex_unlock(&q_mx);
}
bool rq_paint_window(void) { return atomic_load(&q_done) < atomic_load(&prio_window); }
void rq_ask_stats(uint64_t *served, uint64_t *early) { *served = prio_served, *early = prio_early; }
static void *render_thread(void *arg) {
    (void)arg;
    RT_THREAD_NAME("render");
    on_thread = true;
    { extern void sampler_render_thread(void); sampler_render_thread(); }
    for (;;) {
        pthread_mutex_lock(&q_mx);
        for (;;) {
            if (prio.pending) {
                if (!prio.scanned) {  // (which of the queued items may still write a paint map: the last of them)
                    static int types[RQ_N];
                    static uint64_t as[RQ_N], tickets[RQ_N];
                    int n = 0;
                    for (uint64_t i = q_head; i != q_tail && n < RQ_N; i++) {
                        const RqItem *x = &q[i % RQ_N];
                        if (x->ticket > prio.limit) break;
                        types[n] = x->type, as[n] = x->a, tickets[n] = x->ticket, n++;
                    }
                    pthread_mutex_unlock(&q_mx);
                    extern uint64_t renderer_paint_last_writer(const int *types, const uint64_t *a, const uint64_t *tickets, int n);
                    uint64_t after = renderer_paint_last_writer(types, as, tickets, n);
                    // (a map asked about for the first time is not known as a paint map yet, so the look through the
                    // lists could not see what writes it: that ask stays behind everything queued)
                    extern bool renderer_paint_known(uint64_t addr);
                    if (prio.type != RQ_CPU_TEXEL || !renderer_paint_known(((const RqCpuTexel *)prio.p)->addr)) after = prio.limit;
                    pthread_mutex_lock(&q_mx);
                    prio.after = after, prio.scanned = true;
                }
                extern bool renderer_paint_upload_pending(void);
                uint64_t done = atomic_load(&q_done);
                if (q_head == q_tail || done >= prio.limit || (done >= prio.after && !renderer_paint_upload_pending())) {
                    int type = prio.type;
                    void *p = prio.p;
                    bool early = q_head != q_tail && done < prio.limit;
                    pthread_mutex_unlock(&q_mx);
                    if (type == RQ_CPU_TEXEL) { extern void renderer_cpu_texel(RqCpuTexel *); uint64_t t0 = plat_now_ns(); renderer_cpu_texel(p); ask_note(p, plat_now_ns() - t0); }
                    pthread_mutex_lock(&q_mx);
                    ask_stats_log();
                    prio.pending = false;
                    atomic_store(&ask_waiting, 0);
                    prio_served++, prio_early += early;
                    if (early) atomic_store(&prio_window, prio.limit);  // (tests: writes to a paint map before this is done were missed)
                    pthread_cond_broadcast(&q_done_cv);
                    continue;
                }
            }
            if (q_head != q_tail) break;
            pthread_cond_wait(&q_cv, &q_mx);
        }
        RqItem it = q[q_head % RQ_N];
        pthread_mutex_unlock(&q_mx);
        // ODYSSEY_LOG_RENDER_TIME=1: its work per frame (the lists' translation and submission; the frame hand-over; the
        // present, which waits for the window) - with ODYSSEY_LOG_GAME_TIME, what a frame costs the CPU (Steam Deck)
        static int rt_log = -1;
        static uint64_t rt_ns[3];
        static int rt_n;
        if (rt_log < 0) rt_log = getenv("ODYSSEY_LOG_RENDER_TIME") != NULL;
        uint64_t t0 = rt_log ? plat_now_ns() : 0;
        switch (it.type) {
        case RQ_EXEC: backend_execute(it.a); if (midv_n) midv_check(); break;
        case RQ_FENCE: {
            extern uint64_t renderer_guest_write_seq(void);
            NvnSync *s = it.p;
            s->seq = renderer_fence_seq();
            extern bool renderer_fence_point(uint64_t *mark);
            s->gw_ok = renderer_fence_point(&s->gw_mark);  // (guarded compute writes: may end the submission)
            s->write_seq = renderer_guest_write_seq();
            __atomic_store_n(&s->signaled, 1, __ATOMIC_RELEASE);
            break;
        }
        case RQ_FRAME: backend_frame_done(it.p, it.a); break;
        case RQ_PRESENT: backend_present(it.p); atomic_fetch_sub(&q_presents, 1); break;
        case RQ_ODYSSEY_POSE: odyssey_pose_render_frame(it.p); break;
#ifndef __APPLE__
        case RQ_EXTRA_FRAME: { extern void renderer_extra_frame(bool on); renderer_extra_frame(it.a != 0); break; }
#endif
#ifndef __APPLE__
        case RQ_CPU_TEXEL: { extern void renderer_cpu_texel(RqCpuTexel *); renderer_cpu_texel(it.p); break; }
        case RQ_PAINT_WRITE: {
            extern void renderer_paint_cpu_written(uint64_t, uint64_t);
            RqPaintWrite *w = it.p; renderer_paint_cpu_written(w->addr, w->size);
            if (w->heap) free(w);
            break;
        }
#endif
#ifndef __APPLE__  // (the temporal upscalers: every Vulkan build)
        case RQ_DLSS_CAMERA: vk_dlss_set_camera(it.p); free(it.p); break;
#endif
        }
        if (rt_log) {
            rt_ns[it.type == RQ_PRESENT ? 2 : it.type == RQ_FRAME ? 1 : 0] += plat_now_ns() - t0;
            if (it.type == RQ_PRESENT && ++rt_n == 600) {
                LOG("[render-time] lists %.2f ms, frame hand-over %.2f ms, present %.2f ms a frame\n", rt_ns[0] / 600.0 / 1e6,
                    rt_ns[1] / 600.0 / 1e6, rt_ns[2] / 600.0 / 1e6);
                rt_ns[0] = rt_ns[1] = rt_ns[2] = 0, rt_n = 0;
            }
        }
        // (list records the game overwrote meanwhile: freed once nothing queued then, nor a replay, can reach them)
        extern void cmd_reap(uint64_t done);
        extern bool interp_replay_busy(void);
        if (!interp_replay_busy()) cmd_reap(it.ticket);
        pthread_mutex_lock(&q_mx);
        q_head++;
        atomic_store(&q_done, it.ticket);
        pthread_cond_broadcast(&q_done_cv);
        pthread_mutex_unlock(&q_mx);
    }
    return NULL;
}

static pthread_once_t rq_once = PTHREAD_ONCE_INIT;  // (one render thread: rq_push and rq_ask share it)
static void rq_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, render_thread, NULL) == 0) pthread_detach(t);
}
uint64_t rq_push(int type, void *p, uint64_t a) {
    pthread_once(&rq_once, rq_start);
    if (type == RQ_PRESENT) atomic_fetch_add(&q_presents, 1);
    pthread_mutex_lock(&q_mx);
    if (q_tail - q_head >= RQ_N) {  // (full: the thread is far behind)
        kernel_block_begin();
        while (q_tail - q_head >= RQ_N) pthread_cond_wait(&q_done_cv, &q_mx);
    }
    uint64_t t = ++q_ticket;
    q[q_tail % RQ_N] = (RqItem){type, p, a, t};
    q_tail++;
    pthread_cond_signal(&q_cv);
    pthread_mutex_unlock(&q_mx);
    return t;
}

// The game's poison checks (PaintObj) ask for the paint map as the GPU has it. Queued behind everything, the ask made
// the game thread wait until the render thread had worked through the whole of the last frame's lists - on a Steam
// Deck ~7 ms a frame, the two threads then running one after the other (the owner's Wooded Kingdom recording there:
// 36-61 FPS with every other wait gone, a locked 60 with the readback off). The ask only needs what may write a
// paint map to be done: the render thread looks through the queued lists for that (renderer_paint_last_writer),
// runs the ask right after the last such item - and not while CPU edits handed back still wait to be uploaded -
// and what is queued behind goes on afterwards as it would have.
uint64_t rq_ask(int type, void *p) {
    static pthread_mutex_t ask_mx = PTHREAD_MUTEX_INITIALIZER;
    pthread_once(&rq_once, rq_start);
    kernel_block_begin();
    pthread_mutex_lock(&ask_mx);
    pthread_mutex_lock(&q_mx);
    prio.type = type, prio.p = p, prio.limit = q_ticket, prio.after = 0, prio.scanned = false, prio.pending = true;
    prio.seq = ++ask_seq;
    atomic_store(&ask_waiting, prio.seq);
    uint64_t limit = prio.limit;
    uint64_t t0 = plat_now_ns();
    pthread_cond_signal(&q_cv);
    while (prio.pending) pthread_cond_wait(&q_done_cv, &q_mx);
    atomic_fetch_add(&ask_wait_ns, plat_now_ns() - t0);
    pthread_mutex_unlock(&q_mx);
    pthread_mutex_unlock(&ask_mx);
    return limit;
}

bool rq_is_done(uint64_t ticket) { return atomic_load(&q_done) >= ticket; }
uint64_t rq_done_ticket(void) { return atomic_load(&q_done); }

uint64_t rq_last_ticket(void) {
    pthread_mutex_lock(&q_mx);
    uint64_t t = q_ticket;
    pthread_mutex_unlock(&q_mx);
    return t;
}

void rq_wait(uint64_t ticket) {
    if (on_thread || atomic_load(&q_done) >= ticket) return;
    kernel_block_begin();  // (the core's other threads run meanwhile: kernel.c core gates)
    pthread_mutex_lock(&q_mx);
    while (atomic_load(&q_done) < ticket) pthread_cond_wait(&q_done_cv, &q_mx);
    pthread_mutex_unlock(&q_mx);
}

void rq_drain(void) {
    if (on_thread || !rq_enabled()) return;
    pthread_mutex_lock(&q_mx);
    uint64_t t = q_ticket;
    pthread_mutex_unlock(&q_mx);
    rq_wait(t);
}

// (a reboot: what the old game queued runs out before it goes)
RT_ON_RESET(rq_reset) { rq_drain(); }

void rq_present_throttle(void) {
    if (on_thread || atomic_load(&q_presents) < 1) return;
    kernel_block_begin();
    pthread_mutex_lock(&q_mx);
    while (atomic_load(&q_presents) >= 1) pthread_cond_wait(&q_done_cv, &q_mx);
    pthread_mutex_unlock(&q_mx);
}
