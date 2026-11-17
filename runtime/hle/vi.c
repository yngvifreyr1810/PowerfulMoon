// nn::vi (display/layer) and nv:: (graphics driver init), implemented on the host.
#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "../hle.h"
#include "../kernel.h"

static uint32_t vsync_r, vsync_w;
static KEvent *vsync_event;
uint32_t vi_vsync_handle(void) { return vsync_r; }
volatile uint64_t g_vsync_count;  // vsyncs signalled so far (runtime/hle/framepace.c)
uint64_t g_native_window;
static pthread_mutex_t vsync_lock = PTHREAD_MUTEX_INITIALIZER;  // (host waiters: vi_wait_vsync)
static pthread_cond_t vsync_cond = PTHREAD_COND_INITIALIZER;
#define VSYNC_NS 16666667ull
static uint64_t base_ns, base_count;  // (vsync_lock) vsync number base_count + k is due at base_ns + k periods

// Locked to the display (ODYSSEY_VSYNC_LOCK=0/1, on by default on Linux, where pacing.c's present timing is):
// runtime/nvn/vulkan/pacing.c reports when presented images reached the screen. Their spacing, filtered, is the
// display's refresh as the game sees it (60 Hz at a 60 or 120 Hz display); the timer then ticks at that period
// instead of its own, nudged (at most 0.25 ms a tick) until its ticks fall a fixed time after the reported screen
// updates (ODYSSEY_VSYNC_LOCK_MS, default 9). Unlocked, the timer's phase against the display is wherever it started
// and creeps (~0.03 ms/s on a Deck), so now and then it sits where a frame lands either side of the compositor's
// latch: an image shown twice. Steam Deck, Gaming Mode, 1280x800, 50 s walk (2026-11-15), images held for a second
// refresh: unlocked 5 and 9; locked at 1 / 5 / 9 / 13 ms: 2 / 7 / 0 / 6; at 9 again: 2 and 2 (each a game hitch).
// Off when the reports stop (a pause, a hidden window) or wander (a display whose refresh isn't a multiple of the
// game's: 144 Hz).
static pthread_mutex_t disp_mx = PTHREAD_MUTEX_INITIALIZER;
static double disp_period;      // (disp_mx) ns
static double disp_dev;         // (disp_mx) the samples' mean distance from it, ns
static uint64_t disp_last, disp_n;
void vi_display_vblank(uint64_t t) {
    double want = (double)vsync_period_ns();
    pthread_mutex_lock(&disp_mx);
    if (!disp_period || fabs(disp_period - want) > want * 0.02) disp_period = want, disp_dev = want, disp_n = 0;
    if (disp_last && t > disp_last && t - disp_last < 200000000ull) {
        double dt = (double)(t - disp_last), n = floor(dt / disp_period + 0.5);
        if (n >= 1 && n <= 8) {
            double one = dt / n;
            disp_dev += (fabs(one - disp_period) - disp_dev) / 32;
            disp_period += (one - disp_period) / 64;
            disp_n++;
        }
    }
    disp_last = t;
    pthread_mutex_unlock(&disp_mx);
}
// (the vsync thread) the display's period and the latest time it showed an image, or false: not locked
static bool disp_lock(uint64_t now, double *period, uint64_t *last) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("ODYSSEY_VSYNC_LOCK");
        on = !e || atoi(e);  // (Windows too, since 2026-11-16: the game's timer kept in step with the display)
    }
    if (!on) return false;
    pthread_mutex_lock(&disp_mx);
    bool ok = disp_n >= 120 && disp_dev < 1000000 && disp_last && now - disp_last < 500000000ull;
    *period = disp_period, *last = disp_last;
    pthread_mutex_unlock(&disp_mx);
    return ok;
}
// Where t falls on the timer's grid: -period/2 .. period/2 from the nearest tick (pacing.c's log)
int64_t vi_vsync_phase_ns(uint64_t t) {
    pthread_mutex_lock(&vsync_lock);
    uint64_t b = base_ns, p = vsync_period_ns();
    pthread_mutex_unlock(&vsync_lock);
    if (!b || !p) return 0;
    int64_t d = (int64_t)(t - b) % (int64_t)p;
    if (d < 0) d += (int64_t)p;
    return d > (int64_t)p / 2 ? d - (int64_t)p : d;
}

static void *vsync_thread(void *arg) {
    (void)arg;
    uint64_t period = VSYNC_NS;
    plat_thread_realtime(period);
    uint64_t next = plat_now_ns();
    pthread_mutex_lock(&vsync_lock);
    base_ns = next, base_count = g_vsync_count;
    pthread_mutex_unlock(&vsync_lock);
    for (;;) {
        uint64_t want = vsync_period_ns();  // (the menu's Frame rate: 60, 90 or 120 Hz, changed while the game runs)
        if (want != period) {
            period = want;
            pthread_mutex_lock(&vsync_lock);
            base_ns = next, base_count = g_vsync_count;
            pthread_mutex_unlock(&vsync_lock);
        }
        double dp;
        uint64_t dlast;
        if (disp_lock(next, &dp, &dlast)) {
            static double frac;  // (the period's fraction of a ns, carried)
            static int64_t lead = -1;
            if (lead < 0) lead = getenv("ODYSSEY_VSYNC_LOCK_MS") ? (int64_t)(atof(getenv("ODYSSEY_VSYNC_LOCK_MS")) * 1e6) : 9000000;
            frac += dp;
            uint64_t step = (uint64_t)frac;
            frac -= (double)step;
            next += step;
            int64_t P = (int64_t)dp, e = ((int64_t)(next - dlast) - lead) % P;  // (how far past the wanted point)
            if (e < 0) e += P;
            if (e > P / 2) e -= P;
            int64_t adj = -e / 8;
            if (adj > 250000) adj = 250000;
            if (adj < -250000) adj = -250000;
            next += adj;
            static bool said;
            if (!said) { said = true; LOG("[vi] vsync timer locked to the display: %.4f ms a refresh\n", dp / 1e6); }
        } else next += period;
        plat_sleep_until_ns(next);
        uint64_t now = plat_now_ns();
        bool late = now > next + period * 3;
        if (late) {
            static int warned;
            if (warned++ < 20) LOG("[vi] vsync timer late by %llums (host throttling?)\n", (unsigned long long)((now - next) / 1000000));
            next = now;  // don't burst-signal to catch up
        }
        pthread_mutex_lock(&vsync_lock);
        __atomic_add_fetch(&g_vsync_count, 1, __ATOMIC_RELAXED);
        base_ns = next, base_count = g_vsync_count;  // (each tick: a locked timer's period isn't vsync_period_ns)
        pthread_cond_broadcast(&vsync_cond);
        pthread_mutex_unlock(&vsync_lock);
        KEvent *ev = __atomic_load_n(&vsync_event, __ATOMIC_ACQUIRE);
        if (ev) {
            pthread_mutex_lock(&g_klock);
            kernel_signal(ev);
            pthread_mutex_unlock(&g_klock);
        }
    }
    return NULL;
}

// The 60 Hz display timer runs from the first use on: the game's vsync event or NVN's present pacing
// (runtime/nvn/objects.c nvnQueuePresentTexture).
static void vsync_start_thread(void) {
    pthread_t t;
    pthread_create(&t, NULL, vsync_thread, NULL);
}
static void vsync_start(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, vsync_start_thread);
}

uint64_t vi_vsync_count(void) {
    vsync_start();
    return __atomic_load_n(&g_vsync_count, __ATOMIC_RELAXED);
}

// Blocks the calling (host or guest) thread until vsync number `target`; returns the count then.
uint64_t vi_wait_vsync(uint64_t target) {
    vsync_start();
    pthread_mutex_lock(&vsync_lock);
    while (__atomic_load_n(&g_vsync_count, __ATOMIC_RELAXED) < target) pthread_cond_wait(&vsync_cond, &vsync_lock);
    uint64_t n = g_vsync_count;
    pthread_mutex_unlock(&vsync_lock);
    return n;
}

// When vsync number `count` is due (host ns; the timer's schedule - it signals a little after). Frame interpolation
// (runtime/nvn/vulkan/device.c) places a frame's images by the vsync its present waits for.
uint64_t vi_vsync_time(uint64_t count) {
    vsync_start();
    pthread_mutex_lock(&vsync_lock);
    uint64_t t = count >= base_count ? base_ns + (count - base_count) * VSYNC_NS : base_ns;
    pthread_mutex_unlock(&vsync_lock);
    return base_ns ? t : plat_now_ns();  // (the timer thread not started yet)
}

HLE_FUNC("_ZN2nn2vi10InitializeEv", vi_initialize) { (void)c; }
HLE_FUNC("_ZN2nn2vi8FinalizeEv", vi_finalize) { (void)c; }

HLE_FUNC("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_default_display) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", vi_close_display) { (void)c; }

HLE_FUNC("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", vi_destroy_layer) { (void)c; }
HLE_FUNC("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", vi_set_layer_crop) { c->x[0] = 0; }
// (Odyssey; the output scaling is the renderer's)
HLE_FUNC("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", vi_set_layer_scaling_mode) { c->x[0] = 0; }

HLE_FUNC("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window) {
    if (!g_native_window) g_native_window = (uint64_t)hle_alloc(0x100);
    *(uint64_t *)(uintptr_t)c->x[0] = g_native_window;
    c->x[0] = 0;
}

// Reboot: the vsync timer and its event live on (the kernel's handle table doesn't): new handles at the next boot.
RT_ON_RESET(vi_reset) {
    vsync_r = vsync_w = 0;
    g_native_window = 0;  // (HLE heap)
}

HLE_FUNC("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_get_vsync_event) {
    if (!vsync_event) {
        __atomic_store_n(&vsync_event, kernel_create_event_pair(&vsync_w, &vsync_r), __ATOMIC_RELEASE);
        vsync_start();
    } else if (!vsync_r) {
        vsync_w = kernel_new_handle(vsync_event);
        vsync_r = kernel_new_handle(vsync_event);
    }
    // nn::os::AttachReadableHandleToSystemEvent(event, handle, managed=false, clear_mode=AutoClear)
    c->x[1] = vsync_r;
    c->x[2] = 0;
    c->x[3] = 1;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nv18InitializeGraphicsEPvm", nv_initialize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv16FinalizeGraphicsEv", nv_finalize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_allocator) { (void)c; }
HLE_FUNC("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_dt_allocator) { (void)c; }
