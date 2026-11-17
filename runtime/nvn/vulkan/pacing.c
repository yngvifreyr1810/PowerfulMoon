// Display timing: when each presented image actually reached the screen (VK_KHR_present_id + VK_KHR_present_wait).
//
// The game's 60 Hz is a CPU timer (runtime/hle/vi.c) and presents go out in mailbox mode, so nothing ties a frame to
// the display's own refresh: a timer a hair off the panel's rate, or a phase right at the compositor's latch point,
// shows one frame twice and drops another now and then - a hitch the game's own frame times never show. This file
// measures it: a thread waits for each present id in turn and notes when it completed. Two completions within a few
// ms of each other: the first image was replaced before it was shown (dropped). A gap of 1.5 refreshes or more: the
// display showed the previous image again (held). The times of the images shown also go to vi.c, which can lock its
// timer to them (ODYSSEY_VSYNC_LOCK).
//
// Log every 10 s ([display]): images shown, dropped, held, the spacing of the shown ones (p50 / p99 / max), the time
// from the present call to the screen, and where on the timer's 60 Hz grid they landed (phase; drifting = the timer
// and the display disagree). On by default on Linux where the driver has both extensions; ODYSSEY_VK_PRESENT_WAIT=0/1.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "../../platform.h"
#include "vk.h"

bool g_pw_enabled;  // (device.c: the features were enabled)
static PFN_vkWaitForPresentKHR wait_fn;
static _Atomic uint64_t pw_given;   // the last present id handed out
static uint64_t pw_done;            // (the thread) the last id it saw complete
static uint64_t pw_call[256];       // when each id's present was called (id % 256)
static uint64_t pw_gpu[256];        // (ODYSSEY_LOG_PRESENT_GPU) when its submission finished on the GPU
void vk_pacing_gpu_done(uint64_t val, uint64_t t) { pw_gpu[val % 256] = t; }
static pthread_mutex_t pw_mx = PTHREAD_MUTEX_INITIALIZER;  // held while waiting on a swapchain: create_swapchain
                                                            // takes it before destroying the old one
static pthread_mutex_t pw_cmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pw_cv = PTHREAD_COND_INITIALIZER;

void vi_display_vblank(uint64_t t);           // runtime/hle/vi.c
int64_t vi_vsync_phase_ns(uint64_t t);        // (where t lands on the timer's grid, -period/2 .. period/2)

static void *pw_thread(void *arg);
bool vk_pacing_want(void) {
    const char *e = getenv("ODYSSEY_VK_PRESENT_WAIT");
    return !e || atoi(e);  // (Windows too, since 2026-11-16: the display lock below needs it; NVIDIA / AMD drivers have it)
}
void vk_pacing_init(void) {
    if (!g_pw_enabled) return;
    wait_fn = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(g_vk.dev, "vkWaitForPresentKHR");
    if (!wait_fn) { g_pw_enabled = false; return; }
    pthread_t th;
    if (pthread_create(&th, NULL, pw_thread, NULL) == 0) pthread_detach(th);
    else g_pw_enabled = false;
    LOG("[display] present timing: %s\n", g_pw_enabled ? "on (present wait)" : "off");
}

// (present lock) a present id for the present about to be made: chained into pi through *pid / *val (the caller's)
void vk_pacing_tag(VkPresentInfoKHR *pi, VkPresentIdKHR *pid, uint64_t *val) {
    if (!g_pw_enabled) return;
    *val = atomic_load(&pw_given) + 1;
    *pid = (VkPresentIdKHR){.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .pNext = pi->pNext, .swapchainCount = 1, .pPresentIds = val};
    pi->pNext = pid;
    pw_call[*val % 256] = plat_now_ns();
}
// (present lock) after the present call: the id is in flight (or never will be: failed)
void vk_pacing_presented(uint64_t val, bool ok) {
    if (!g_pw_enabled || !val) return;
    if (!ok) pw_call[val % 256] = 0;
    pthread_mutex_lock(&pw_cmx);
    atomic_store(&pw_given, val);
    pthread_cond_signal(&pw_cv);
    pthread_mutex_unlock(&pw_cmx);
}
// (present lock) the swapchain old is about to be destroyed: not waited on any more
void vk_pacing_retire(void) {
    if (!g_pw_enabled) return;
    pthread_mutex_lock(&pw_mx);
    pthread_mutex_unlock(&pw_mx);
}

// ---- the stats of a 10 s window
#define NS 1024
static struct {
    uint64_t t0, shown, dropped, held, lost;
    float iv[NS];  // spacing of shown images, ms
    int niv;
    double lat_sum, lat_max;
    double ph_sum, gpu_sum;
    int nlat, ngpu;
    uint64_t last_shown;
} st;
static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static void st_report(uint64_t now) {
    float p50 = 0, p99 = 0, mx = 0;
    if (st.niv) {
        qsort(st.iv, (size_t)st.niv, sizeof st.iv[0], cmpf);
        p50 = st.iv[st.niv / 2], p99 = st.iv[(st.niv * 99) / 100], mx = st.iv[st.niv - 1];
    }
    LOG("[display] %.0fs: %llu shown, %llu dropped, %llu held (%llu unknown); spacing p50 %.2f p99 %.2f max %.2f ms; "
        "present to screen %.1f ms (max %.1f; GPU done after %.1f); timer phase %+.2f ms\n",
        (now - st.t0) / 1e9, (unsigned long long)st.shown, (unsigned long long)st.dropped, (unsigned long long)st.held,
        (unsigned long long)st.lost, p50, p99, mx, st.nlat ? st.lat_sum / st.nlat : 0, st.lat_max, st.ngpu ? st.gpu_sum / st.ngpu : 0,
        st.nlat ? st.ph_sum / st.nlat / 1e6 : 0);
    uint64_t last = st.last_shown;
    memset(&st, 0, sizeof st);
    st.t0 = now, st.last_shown = last;
}

// The performance overlay's frame time graph (runtime/ui/menu.mm): the spacing of the images the display showed,
// when known - a held image is the spike the eye sees. -1: not known (no present timing, or nothing shown lately).
static _Atomic uint64_t disp_ms_bits, disp_ms_at;
float vk_display_frame_ms(void) {
    if (!g_pw_enabled || plat_now_ns() - atomic_load(&disp_ms_at) > 250000000ull) return -1;
    uint64_t b = atomic_load(&disp_ms_bits);
    float f;
    memcpy(&f, &b, sizeof f);
    return f;
}
static uint64_t g_refresh_ns = 16666667;  // (the thread) the spacing of shown images, filtered: the display's refresh
static void note(uint64_t id, uint64_t t, bool ok) {
    if (!st.t0) st.t0 = t;
    if (!ok) st.lost++;
    else if (st.last_shown && t - st.last_shown < 3000000) st.dropped++;  // (completed together with the next one)
    else {
        st.shown++;
        if (st.last_shown) {
            uint64_t dt = t - st.last_shown;
            if (dt < 200000000ull) {  // (not across a pause)
                if (st.niv < NS) st.iv[st.niv++] = (float)(dt / 1e6);
                float f = (float)(dt / 1e6);
                uint64_t b = 0;
                memcpy(&b, &f, sizeof f);
                atomic_store(&disp_ms_bits, b), atomic_store(&disp_ms_at, t);
                if (dt * 2 >= g_refresh_ns * 3) st.held += (dt + g_refresh_ns / 2) / g_refresh_ns - 1;
                else g_refresh_ns += ((int64_t)dt - (int64_t)g_refresh_ns) / 64;
            }
        }
        st.last_shown = t;
        uint64_t call = pw_call[id % 256];
        if (call && t > call && t - call < 500000000ull) {
            double l = (t - call) / 1e6;
            st.lat_sum += l, st.nlat++;
            if (l > st.lat_max) st.lat_max = l;
            st.ph_sum += (double)vi_vsync_phase_ns(t);
            uint64_t g = pw_gpu[id % 256];
            if (g > call && g < t + 100000000ull) st.gpu_sum += (g - call) / 1e6, st.ngpu++;
            pw_gpu[id % 256] = 0;
        }
        vi_display_vblank(t);
    }
    if (t - st.t0 >= 10000000000ull) st_report(t);
}

static void *pw_thread(void *arg) {
    (void)arg;
    plat_thread_realtime(0);  // (the completion times are the point: woken promptly)
    uint64_t tries = 0;
    for (;;) {
        pthread_mutex_lock(&pw_cmx);
        while (atomic_load(&pw_given) <= pw_done) pthread_cond_wait(&pw_cv, &pw_cmx);
        pthread_mutex_unlock(&pw_cmx);
        uint64_t k = pw_done + 1;
        if (!pw_call[k % 256]) { pw_done = k; tries = 0; continue; }  // (its present failed or was skipped)
        pthread_mutex_lock(&pw_mx);
        VkSwapchainKHR sc = g_vk.swapchain;
        VkResult r = sc ? wait_fn(g_vk.dev, sc, k, 5000000ull) : VK_ERROR_OUT_OF_DATE_KHR;
        pthread_mutex_unlock(&pw_mx);
        uint64_t t = plat_now_ns();
        if (r == VK_TIMEOUT && ++tries < 40) continue;  // (gives up after 200 ms: shown on a swapchain since replaced?)
        note(k, t, r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR);
        pw_done = k;
        tries = 0;
    }
    return NULL;
}
