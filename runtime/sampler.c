// Sampling profiler of the game thread (Linux; a debugging tool, off unless asked for):
//   ODYSSEY_SAMPLE_AT=<frame> ODYSSEY_SAMPLE_FRAMES=<n> (default 600)
// From that presented frame on, for n frames, the thread that presents (the game's main thread) is interrupted about
// 2000 times a second (SIGPROF from a timer thread) and where it was is recorded. Then build/samples.txt lists
// "count module+offset" (most first): the module is the program, the game code library (odyssey_game.so) or a system
// library, the offset an address in that file - names come from its symbols afterwards (nm), as the game code's are
// hidden from dladdr. Where the time goes per frame, in the game's own code or in the runtime around it.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  // (dladdr; before the first include)
#endif
#include "sampler.h"

#ifdef __linux__
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "rt.h"

#define MAXS (1 << 20)
// ODYSSEY_SAMPLE_STACK=1: each sample with its callers (glibc backtrace from the signal handler - the unwind tables;
// warmed up once before sampling. A debugging tool: not strictly signal-safe)
#define STK 14
static int g_stack;
static uintptr_t (*g_stk)[STK];
static uintptr_t *g_pcs;
static _Atomic unsigned g_n;
static volatile int g_on;
static pid_t g_tid;

static void on_prof(int sig, siginfo_t *si, void *uc_) {
    (void)sig, (void)si;
    if (!g_on) return;
    ucontext_t *uc = uc_;
#if defined(__x86_64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
#else
    uintptr_t pc = 0;
#endif
    unsigned i = atomic_fetch_add(&g_n, 1);
    if (i < MAXS) g_pcs[i] = pc;
    if (g_stack && i < MAXS) {
        void *bt[STK + 3];
        int n = backtrace(bt, STK + 3);  // ([0] this handler, [1] the signal trampoline, [2] the interrupted pc)
        for (int k = 0; k < STK; k++) g_stk[i][k] = k + 3 < n ? (uintptr_t)bt[k + 3] : 0;
    }
}

// The process's busiest thread over half a second (its CPU time: /proc/self/task/<tid>/stat utime + stime) - the
// thread that presents mostly waits; the one doing the frame's work may be another (a guest thread, the render thread)
static pid_t busiest_thread(void) {
    enum { MAXT = 512 };
    static pid_t tids[MAXT];
    static unsigned long long t0[MAXT];
    int n = 0;
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task", (int)getpid());
    DIR *d = opendir(path);
    for (struct dirent *e; d && (e = readdir(d)) && n < MAXT;)
        if (e->d_name[0] >= '0' && e->d_name[0] <= '9') tids[n++] = (pid_t)atoi(e->d_name);
    if (d) closedir(d);
    unsigned long long now[MAXT];
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            char p[96], buf[1024];
            snprintf(p, sizeof p, "/proc/%d/task/%d/stat", (int)getpid(), (int)tids[i]);
            FILE *f = fopen(p, "r");
            unsigned long long ut = 0, st = 0;
            if (f && fgets(buf, sizeof buf, f)) {
                char *c = strrchr(buf, ')');  // (fields after the name: state is 3, utime 14, stime 15)
                int field = 2;
                for (char *tok = c ? strtok(c + 1, " ") : NULL; tok; tok = strtok(NULL, " ")) {
                    field++;
                    if (field == 14) ut = strtoull(tok, NULL, 10);
                    if (field == 15) { st = strtoull(tok, NULL, 10); break; }
                }
            }
            if (f) fclose(f);
            (pass ? now : t0)[i] = ut + st;
        }
        if (!pass) { struct timespec ts = {0, 500 * 1000 * 1000}; nanosleep(&ts, NULL); }
    }
    pid_t best = g_tid;
    unsigned long long most = 0;
    for (int i = 0; i < n; i++)
        if (now[i] - t0[i] > most) most = now[i] - t0[i], best = tids[i];
    return best;
}

// ODYSSEY_SAMPLE_THREAD_NAME=<prefix>: the thread whose name (/proc/.../comm: "g83 c0 p45" for guest thread 83,
// "render", ...) starts with it, instead of the busiest one (0: none found)
static pid_t thread_by_name(const char *prefix) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task", (int)getpid());
    DIR *d = opendir(path);
    pid_t found = 0;
    for (struct dirent *e; d && (e = readdir(d)) && !found;) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[96], name[64] = "";
        snprintf(p, sizeof p, "/proc/%d/task/%s/comm", (int)getpid(), e->d_name);
        FILE *f = fopen(p, "r");
        if (f && fgets(name, sizeof name, f) && !strncmp(name, prefix, strlen(prefix))) found = (pid_t)atoi(e->d_name);
        if (f) fclose(f);
    }
    if (d) closedir(d);
    return found;
}

static void *ticker(void *a) {
    (void)a;
    pid_t pid = getpid();
    pid_t presenter = g_tid;
    const char *want = getenv("ODYSSEY_SAMPLE_THREAD_NAME");
    // ("present": the thread that presents - the game's main thread - by its id, not a name)
    pid_t named = want && !strcmp(want, "present") ? presenter : want && *want ? thread_by_name(want) : 0;
    g_tid = named ? named : busiest_thread();
    LOG("[sample] sampling thread %d (the presenting thread is %d)\n", (int)g_tid, (int)presenter);
    struct timespec ts = {0, 500 * 1000};  // (2 kHz)
    while (g_on) {
        syscall(SYS_tgkill, pid, g_tid, SIGPROF);
        nanosleep(&ts, NULL);
    }
    return NULL;
}

typedef struct { uintptr_t pc; unsigned n; } Row;
static int by_pc(const void *a, const void *b) {
    uintptr_t x = ((const Row *)a)->pc, y = ((const Row *)b)->pc;
    return x < y ? -1 : x > y;
}
static int by_n(const void *a, const void *b) { return (int)((const Row *)b)->n - (int)((const Row *)a)->n; }

static void dump(void) {
    unsigned n = atomic_load(&g_n);
    if (n > MAXS) n = MAXS;
    Row *r = malloc(sizeof *r * (n ? n : 1));
    if (g_stack) {  // (the call chains: "count pc <- caller <- ..." per distinct chain, most first)
        FILE *f = fopen("build/stacks.txt", "w");
        typedef struct { uintptr_t k[STK + 1]; unsigned c; } Chain;
        Chain *ch = calloc(n ? n : 1, sizeof *ch);
        unsigned nc = 0;
        for (unsigned i = 0; i < n; i++) {
            uintptr_t key[STK + 1] = {g_pcs[i]};
            memcpy(key + 1, g_stk[i], sizeof g_stk[i]);
            unsigned j = 0;
            for (; j < nc && memcmp(ch[j].k, key, sizeof key); j++) {}
            if (j == nc && nc < 20000) memcpy(ch[nc++].k, key, sizeof key);
            if (j < nc) ch[j].c++;
        }
        for (unsigned a = 0; a < nc; a++)  // (most first: a simple selection, a debugging tool)
            for (unsigned b = a + 1; b < nc; b++) if (ch[b].c > ch[a].c) { Chain t = ch[a]; ch[a] = ch[b]; ch[b] = t; }
        for (unsigned a = 0; f && a < nc && a < 300; a++) {
            fprintf(f, "%u", ch[a].c);
            for (int k = 0; k <= STK && ch[a].k[k]; k++) {
                Dl_info di;
                if (dladdr((void *)ch[a].k[k], &di) && di.dli_fname)
                    fprintf(f, " %s+%#lx", strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname,
                            (unsigned long)(ch[a].k[k] - (uintptr_t)di.dli_fbase));
                else fprintf(f, " ?+%#lx", (unsigned long)ch[a].k[k]);
            }
            fprintf(f, "\n");
        }
        if (f) fclose(f);
        free(ch);
    }
    for (unsigned i = 0; i < n; i++) r[i] = (Row){g_pcs[i], 1};
    qsort(r, n, sizeof *r, by_pc);
    unsigned m = 0;
    for (unsigned i = 0; i < n; i++) {
        if (m && r[m - 1].pc == r[i].pc) r[m - 1].n++;
        else r[m++] = r[i];
    }
    qsort(r, m, sizeof *r, by_n);
    mkdir("build", 0755);
    FILE *f = fopen("build/samples.txt", "w");
    if (f) {
        fprintf(f, "# %u samples of the game thread (~2000/s)\n", n);
        for (unsigned i = 0; i < m; i++) {
            Dl_info di;
            if (dladdr((void *)r[i].pc, &di) && di.dli_fname)
                fprintf(f, "%u %s+%#lx\n", r[i].n, strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname,
                        (unsigned long)(r[i].pc - (uintptr_t)di.dli_fbase));
            else
                fprintf(f, "%u ?+%#lx\n", r[i].n, (unsigned long)r[i].pc);
        }
        fclose(f);
    }
    LOG("[sample] %u samples written to build/samples.txt\n", n);
    free(r);
}

void sampler_frame(uint64_t frame) {
    static long at = -2, frames;
    if (at == -2) {
        const char *e = getenv("ODYSSEY_SAMPLE_AT");
        at = e ? atol(e) : -1;
        frames = getenv("ODYSSEY_SAMPLE_FRAMES") ? atol(getenv("ODYSSEY_SAMPLE_FRAMES")) : 600;
    }
    if (at < 0) return;
    if ((long)frame == at && !g_on) {
        g_pcs = malloc(sizeof *g_pcs * MAXS);
        if (!g_pcs) { at = -1; return; }
        if (getenv("ODYSSEY_SAMPLE_STACK") && (g_stk = calloc(MAXS, sizeof *g_stk))) {
            void *warm[4];
            backtrace(warm, 4);  // (loads the unwinder now, not inside a signal handler)
            g_stack = 1;
        }
        struct sigaction sa = {0};
        sa.sa_sigaction = on_prof;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, NULL);
        g_tid = (pid_t)syscall(SYS_gettid);
        g_on = 1;
        pthread_t t;
        pthread_create(&t, NULL, ticker, NULL);
        pthread_detach(t);
        LOG("[sample] sampling the game thread from frame %ld for %ld frames\n", at, frames);
    } else if (g_on && (long)frame >= at + frames) {
        g_on = 0;
        at = -1;
        dump();
    }
}
#elif defined(_WIN32)
// Windows: the thread is suspended about 2000 times a second and its stack walked (RtlVirtualUnwind, 6 frames); each
// sample is written as "module+offset <- module+offset ..." and the same stacks counted. ODYSSEY_SAMPLE_THREAD=render:
// the render thread (runtime/nvn/rqueue.c) instead of the game thread that presents.
#include <windows.h>
#include <tlhelp32.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define DEPTH 16
#define MAXS (1 << 19)
typedef struct { uintptr_t tid, pc[DEPTH]; int64_t qpc; } Stack;  // (tid: ODYSSEY_SAMPLE_THREAD=all; qpc: when)
#define STACK_KEY (sizeof(uintptr_t) * (1 + DEPTH))  // (what makes two samples the same: tid and pcs, not the time)
// ODYSSEY_SAMPLE_SLOW=<ms>: only the samples taken during frames that took longer (the game thread's presents) are
// written, frame by frame - the stutters of a long sampled stretch of play, not its average
static double g_slow_ms;
#define MAXF (1 << 18)
static int64_t g_fq[MAXF];
static uint64_t g_ff[MAXF];
static int g_nf;
static Stack *g_st;
static volatile LONG g_n;
static volatile int g_on;
static HANDLE g_thread, g_render, g_main;
// ODYSSEY_SAMPLE_THREAD=main: the game's main thread (its logic and scene code: kernel.c), with ODYSSEY_SAMPLE_SLOW a
// whole session long - the samples go round a ring and only those of the slow frames are kept (g_st)
#define RING (1 << 15)
static Stack *g_ring;
static volatile LONG g_rn;
static int g_ring_on;

void sampler_main_thread(void) {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main, 0, FALSE, DUPLICATE_SAME_ACCESS);
}

// (a frame just presented, slow: its samples from the ring into g_st)
static void ring_keep(int64_t from, int64_t to) {
    LONG end = g_rn - 1, i = end;  // (the newest may still be written: left out)
    while (i > 0 && end - i < RING - 2 && g_ring[(i - 1) & (RING - 1)].qpc > from) i--;
    for (; i < end; i++) {
        const Stack *s = &g_ring[i & (RING - 1)];
        if (s->qpc <= from || s->qpc > to) continue;
        if (g_n < MAXS) g_st[g_n++] = *s;
    }
}
static HANDLE g_all[256];  // (=all: every thread of the process but the sampler, ~500 times a second each)
static DWORD g_all_id[256];
static int g_nall;

void sampler_render_thread(void) {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_render, 0, FALSE, DUPLICATE_SAME_ACCESS);
}

static void walk(CONTEXT *ctx, Stack *s, DWORD tid) {
    memset(s, 0, sizeof *s);
    s->tid = tid;
    for (int i = 0; i < DEPTH && ctx->Rip; i++) {
        s->pc[i] = ctx->Rip;
        DWORD64 base;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx->Rip, &base, NULL);
        if (!f) {  // (a leaf function: its return address is at the stack pointer)
            ctx->Rip = *(DWORD64 *)ctx->Rsp;
            ctx->Rsp += 8;
            continue;
        }
        void *handler;
        DWORD64 frame;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx->Rip, f, ctx, &handler, &frame, NULL);
    }
}

static DWORD WINAPI ticker(void *a) {
    (void)a;
    while (g_on) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        for (int k = 0; k < (g_nall ? g_nall : 1); k++) {
            HANDLE h = g_nall ? g_all[k] : g_thread;
            if (SuspendThread(h) == (DWORD)-1) continue;
            CONTEXT ctx = {.ContextFlags = CONTEXT_FULL};
            if (GetThreadContext(h, &ctx)) {
                if (g_ring_on) {
                    LONG i = InterlockedIncrement(&g_rn) - 1;
                    Stack *s = &g_ring[i & (RING - 1)];
                    walk(&ctx, s, 0);
                    s->qpc = now.QuadPart;
                } else {
                    LONG i = InterlockedIncrement(&g_n) - 1;
                    if (i < MAXS) { walk(&ctx, &g_st[i], g_nall ? g_all_id[k] : 0); g_st[i].qpc = now.QuadPart; }
                }
            }
            ResumeThread(h);
        }
        // (~2 kHz: half a millisecond of spinning on the clock between samples)
        LARGE_INTEGER f, t0, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        do { SwitchToThread(); QueryPerformanceCounter(&t); } while ((t.QuadPart - t0.QuadPart) * (g_nall ? (g_slow_ms > 0 ? 250 : 500) : 2000) < f.QuadPart);
    }
    return 0;
}

static void name(uintptr_t pc, char *out, size_t n) {
    extern const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // (game code: the guest function, f_<addr>)
    uint64_t off;
    const char *g = pc ? rt_host_fn_name(pc, &off) : NULL;
    if (g) { snprintf(out, n, "%s+%#llx", g, (unsigned long long)off); return; }
    HMODULE m;
    char path[MAX_PATH];
    if (pc && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &m) &&
        GetModuleFileNameA(m, path, sizeof path)) {
        const char *b = strrchr(path, 92);  // (a backslash)
        snprintf(out, n, "%s+%#llx", b ? b + 1 : path, (unsigned long long)(pc - (uintptr_t)m));
    } else snprintf(out, n, "?+%#llx", (unsigned long long)pc);
}

static int by_stack(const void *a, const void *b) { return memcmp(a, b, STACK_KEY); }

static int by_time(const void *a, const void *b) {
    const Stack *x = a, *y = b;
    if (x->qpc != y->qpc) return x->qpc < y->qpc ? -1 : 1;
    return memcmp(a, b, STACK_KEY);
}

static void dump_slow(void) {
    LONG n = g_n < MAXS ? g_n : MAXS;
    qsort(g_st, (size_t)n, sizeof *g_st, by_time);
    LARGE_INTEGER fr;
    QueryPerformanceFrequency(&fr);
    mkdir("build", 0755);
    FILE *f = fopen("build/samples.txt", "w");
    int slow = 0;
    LONG at = 0;
    for (int k = 1; f && k < g_nf; k++) {
        double ms = (g_fq[k] - g_fq[k - 1]) * 1000.0 / fr.QuadPart;
        while (at < n && g_st[at].qpc <= g_fq[k - 1]) at++;
        if (ms < g_slow_ms) continue;
        slow++;
        fprintf(f, "## frame %llu: %.1f ms\n", (unsigned long long)g_ff[k], ms);
        LONG from = at, to = at;
        while (to < n && g_st[to].qpc <= g_fq[k]) to++;
        qsort(g_st + from, (size_t)(to - from), sizeof *g_st, by_stack);  // (same stacks together, by thread)
        for (LONG i = from; i < to;) {
            LONG j = i;
            while (j < to && !memcmp(&g_st[i], &g_st[j], STACK_KEY)) j++;
            fprintf(f, "%ld T%lu", (long)(j - i), (unsigned long)g_st[i].tid);
            for (int d = 0; d < DEPTH && g_st[i].pc[d]; d++) {
                char b[300];
                name(g_st[i].pc[d], b, sizeof b);
                fprintf(f, "%s%s", d ? " <- " : " ", b);
            }
            fputc('\n', f);
            i = j;
        }
        at = to;
    }
    if (f) fclose(f);
    LOG("[sample] %d frames over %.0f ms of %d, their samples written to build/samples.txt\n", slow, g_slow_ms, g_nf);
}

static void dump(void) {
    if (g_slow_ms > 0) { dump_slow(); return; }
    LONG n = g_n < MAXS ? g_n : MAXS;
    qsort(g_st, (size_t)n, sizeof *g_st, by_stack);
    typedef struct { LONG at, count; } Run;
    Run *runs = malloc(sizeof *runs * (n ? n : 1));
    LONG m = 0;
    for (LONG i = 0; i < n; i++) {
        if (m && !memcmp(&g_st[runs[m - 1].at], &g_st[i], STACK_KEY)) runs[m - 1].count++;
        else runs[m++] = (Run){i, 1};
    }
    for (LONG i = 0; i < m; i++)  // (most first: a small selection sort over the top only)
        for (LONG j = i + 1; j < m && i < 2000; j++) if (runs[j].count > runs[i].count) { Run t = runs[i]; runs[i] = runs[j]; runs[j] = t; }
    mkdir("build", 0755);
    FILE *f = fopen("build/samples.txt", "w");
    if (f) {
        fprintf(f, "# %ld samples (~2000/s), stacks innermost first\n", (long)n);
        for (LONG i = 0; i < m && i < 2000; i++) {
            fprintf(f, "%ld", (long)runs[i].count);
            if (g_st[runs[i].at].tid) fprintf(f, " T%lu", (unsigned long)g_st[runs[i].at].tid);
            for (int k = 0; k < DEPTH; k++) {
                char b[300];
                if (!g_st[runs[i].at].pc[k]) break;
                name(g_st[runs[i].at].pc[k], b, sizeof b);
                fprintf(f, "%s%s", k ? " <- " : " ", b);
            }
            fputc('\n', f);
        }
        fclose(f);
    }
    LOG("[sample] %ld samples written to build/samples.txt\n", (long)n);
    free(runs);
}

void sampler_flush(void) {
    static LONG done;
    if (!g_on || InterlockedExchange(&done, 1)) return;
    g_on = 0;
    Sleep(5);
    dump();
}

// ---- stall watchdog (Windows): ODYSSEY_STALL_MS=<ms>
// A thread looks every millisecond at when the game last presented. When no present came for longer than that, it
// takes a few snapshots of every thread's stack (each suspended briefly, 4 ms apart) while the stall lasts, and once
// presents resume writes them to the log, symbolized, per thread: what each was doing during a real stutter. Nothing
// is sampled otherwise (an always-on sampler made the rare stalls vanish).
#define STALL_SNAPS 12
#define STALL_THREADS 160
static volatile LONG64 g_present_qpc;
static volatile LONG64 g_present_frame;
typedef struct { DWORD tid; uintptr_t pc[STALL_SNAPS][12]; int n; } StallThread;
static StallThread g_stall[STALL_THREADS];
static HANDLE g_stall_h[STALL_THREADS];
static int g_stall_nt;
static HANDLE g_present_thread;  // (the thread that presents: sampled every millisecond through a stall)
#define STALL_MAIN 256
static uintptr_t g_stall_main[STALL_MAIN][12];

static void stall_walk(HANDLE h, uintptr_t out[12]) {
    memset(out, 0, 12 * sizeof out[0]);
    if (SuspendThread(h) == (DWORD)-1) return;
    CONTEXT ctx = {.ContextFlags = CONTEXT_FULL};
    if (GetThreadContext(h, &ctx)) {
        for (int i = 0; i < 12 && ctx.Rip; i++) {
            out[i] = ctx.Rip;
            DWORD64 base;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
            if (!f) { ctx.Rip = *(DWORD64 *)ctx.Rsp; ctx.Rsp += 8; continue; }
            void *handler;
            DWORD64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, f, &ctx, &handler, &frame, NULL);
        }
    }
    ResumeThread(h);
}

static DWORD WINAPI stall_watch(void *a) {
    double ms = *(double *)a;
    LARGE_INTEGER fr;
    QueryPerformanceFrequency(&fr);
    int64_t limit = (int64_t)(ms * fr.QuadPart / 1000.0);
    int reports = 0;
    unsigned refresh = 0;
    for (;;) {
        Sleep(1);
        int64_t last = g_present_qpc;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (++refresh % 2000 == 1) {  // (the threads opened ahead, every ~2 s: a stall is sampled at once)
            for (int k = 0; k < g_stall_nt; k++) CloseHandle(g_stall_h[k]);
            g_stall_nt = 0;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te = {.dwSize = sizeof te};
            DWORD self = GetCurrentThreadId();
            for (BOOL ok = Thread32First(snap, &te); ok && g_stall_nt < STALL_THREADS; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (!h) continue;
                g_stall[g_stall_nt].tid = te.th32ThreadID;
                g_stall_h[g_stall_nt++] = h;
            }
            CloseHandle(snap);
            continue;
        }
        if (!last || now.QuadPart - last < limit || reports >= 40) continue;
        // a stall: every thread of the process (the list opened ahead, below), a few times
        int nt = g_stall_nt;
        HANDLE *hs = g_stall_h;
        for (int k = 0; k < nt; k++) g_stall[k].n = 0;
        int64_t frame = g_present_frame;
        int snaps = 0, nmain = 0;
        if (g_present_qpc == last) {  // (one snapshot of every thread, then the presenting thread alone, every ~1 ms)
            for (int k = 0; k < nt; k++) stall_walk(hs[k], g_stall[k].pc[g_stall[k].n++]);
            snaps++;
        }
        while (g_present_qpc == last && nmain < STALL_MAIN && g_present_thread) {
            stall_walk(g_present_thread, g_stall_main[nmain++]);
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            do { SwitchToThread(); QueryPerformanceCounter(&t1); } while ((t1.QuadPart - t0.QuadPart) * 1000 < fr.QuadPart);
        }
        while (g_present_qpc == last) Sleep(1);  // (until presents resume: then the log)
        QueryPerformanceCounter(&now);
        double total = (g_present_qpc - last) * 1000.0 / fr.QuadPart;
        reports++;
        LOG("[stall] after frame %lld: %.0f ms without a present, %d snapshots of %d threads, %d of the presenting one\n",
            (long long)frame, total, snaps, nt, nmain);
        // the presenting thread's samples: each distinct stack once, with its count
        for (int i = 0; i < nmain; i++) {
            bool dup = false;
            for (int q = 0; q < i && !dup; q++) dup = !memcmp(g_stall_main[q], g_stall_main[i], sizeof g_stall_main[0]);
            if (dup) continue;
            int count = 0;
            for (int q = i; q < nmain; q++) count += !memcmp(g_stall_main[q], g_stall_main[i], sizeof g_stall_main[0]);
            char line[2400];
            int len = snprintf(line, sizeof line, "[stall]   main x%d:", count);
            for (int d = 0; d < 12 && g_stall_main[i][d]; d++) {
                char b[300];
                name(g_stall_main[i][d], b, sizeof b);
                if (len < (int)sizeof line - 2) len += snprintf(line + len, sizeof line - len, " %s", b);
            }
            LOG("%s\n", line);
        }
        for (int k = 0; k < nt; k++) {
            // one line per distinct stack of this thread (with how many snapshots had it); threads whose every snapshot
            // was the same idle wait are left out (their first frame in ntdll and no game or runtime code below it)
            for (int sidx = 0; sidx < g_stall[k].n; sidx++) {
                bool dup = false;
                for (int q = 0; q < sidx && !dup; q++) dup = !memcmp(g_stall[k].pc[q], g_stall[k].pc[sidx], sizeof g_stall[k].pc[0]);
                if (dup) continue;
                int count = 0;
                for (int q = 0; q < g_stall[k].n; q++) count += !memcmp(g_stall[k].pc[q], g_stall[k].pc[sidx], sizeof g_stall[k].pc[0]);
                char line[2400];
                int len = snprintf(line, sizeof line, "[stall]   T%lu x%d:", (unsigned long)g_stall[k].tid, count);
                bool ours = false;
                for (int d = 0; d < 12 && g_stall[k].pc[sidx][d]; d++) {
                    char b[300];
                    name(g_stall[k].pc[sidx][d], b, sizeof b);
                    if (!strncmp(b, "f_", 2) || !strncmp(b, "SuperOdysseyRecomp", 18)) ours = true;
                    if (len < (int)sizeof line - 2) len += snprintf(line + len, sizeof line - len, " %s", b);
                }
                if (ours) LOG("%s\n", line);
            }
        }
    }
    return 0;
}

void sampler_frame(uint64_t frame) {
    static long at = -2, frames;
    {   // (the stall watchdog's clock)
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        g_present_frame = (LONG64)frame;
        g_present_qpc = q.QuadPart;
        if (!g_present_thread)
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_present_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
        static int started;
        static double stall_ms;
        if (!started) {
            started = 1;
            stall_ms = getenv("ODYSSEY_STALL_MS") ? atof(getenv("ODYSSEY_STALL_MS")) : 0;
            if (stall_ms > 0) CloseHandle(CreateThread(NULL, 0, stall_watch, &stall_ms, 0, NULL));
        }
    }
    if (at == -2) {
        const char *e = getenv("ODYSSEY_SAMPLE_AT");
        at = e ? atol(e) : -1;
        frames = getenv("ODYSSEY_SAMPLE_FRAMES") ? atol(getenv("ODYSSEY_SAMPLE_FRAMES")) : 600;
    }
    if (at < 0) return;
    if (g_on && g_nf < MAXF) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        g_fq[g_nf] = q.QuadPart, g_ff[g_nf++] = frame;
        if (g_ring_on && g_nf > 1) {
            static LARGE_INTEGER fr;
            if (!fr.QuadPart) QueryPerformanceFrequency(&fr);
            if ((g_fq[g_nf - 1] - g_fq[g_nf - 2]) * 1000.0 / fr.QuadPart >= g_slow_ms) ring_keep(g_fq[g_nf - 2], g_fq[g_nf - 1]);
        }
    }
    if ((long)frame == at && !g_on) {
        g_slow_ms = getenv("ODYSSEY_SAMPLE_SLOW") ? atof(getenv("ODYSSEY_SAMPLE_SLOW")) : 0;
        g_st = malloc(sizeof *g_st * MAXS);
        if (!g_st) { at = -1; return; }
        const char *w = getenv("ODYSSEY_SAMPLE_THREAD");
        if (w && !strcmp(w, "render") && g_render) g_thread = g_render;
        else if (w && !strcmp(w, "main") && g_main) {
            g_thread = g_main;
            if (g_slow_ms > 0 && (g_ring = calloc(RING, sizeof *g_ring))) g_ring_on = 1;
        }
        else if (w && !strcmp(w, "all")) {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te = {.dwSize = sizeof te};
            for (BOOL ok = Thread32First(snap, &te); ok && g_nall < 256; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (h) g_all_id[g_nall] = te.th32ThreadID, g_all[g_nall++] = h;
            }
            CloseHandle(snap);
            g_thread = g_all[0];
        }
        else DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
        g_on = 1;
        CloseHandle(CreateThread(NULL, 0, ticker, NULL, 0, NULL));
        LOG("[sample] sampling the %s thread from frame %ld for %ld frames\n",
            g_thread == g_render ? "render" : g_thread == g_main ? "game's main" : "game", at, frames);
    } else if (g_on && (long)frame >= at + frames) {
        g_on = 0;
        at = -1;
        Sleep(5);
        dump();
    }
}
#else
void sampler_frame(uint64_t frame) { (void)frame; }
#endif
#if !defined(_WIN32)
void sampler_render_thread(void) {}
void sampler_main_thread(void) {}
void sampler_flush(void) {}
#endif
