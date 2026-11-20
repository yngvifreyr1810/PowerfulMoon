#include "diagnostics.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <tlhelp32.h>
#endif
#include "paths.h"
#include "platform.h"
#include "rt.h"
#include "settings.h"
#include "version.h"

static char gpu[1024], fault[256], fault_short[128];  // (short: what and the address - no host addresses, for signatures)
static atomic_bool gpu_ready, fault_ready;
static atomic_flag fault_claimed = ATOMIC_FLAG_INIT;
static atomic_uint sequence;
static atomic_int copy_status = -1;
int diagnostics_copy_status(void) { return atomic_load(&copy_status); }
static char build[128] = "unavailable";
static char where_sym[160];        // the faulting code's symbol (dispatch.c rt_fault_report): f_<guest>, module, function
static uint64_t where_off;
static atomic_bool where_ready;
static uint64_t start_ns;          // (the session's length in a report)
static char parent[96] = "unknown";  // what started the game: Explorer, a launcher, Steam...

void diagnostics_fault_where(const char *sym, uint64_t off) {
    snprintf(where_sym, sizeof where_sym, "%s", sym && *sym ? sym : "?");
    where_off = off;
    atomic_store(&where_ready, true);
}

// The process that started this one (a launcher, Steam, Explorer, a terminal): many reports come from launchers
static void parent_name(void) {
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32 e = {.dwSize = sizeof e};
    DWORD me = GetCurrentProcessId(), ppid = 0;
    for (BOOL ok = Process32First(snap, &e); ok; ok = Process32Next(snap, &e))
        if (e.th32ProcessID == me) { ppid = e.th32ParentProcessID; break; }
    e.dwSize = sizeof e;
    for (BOOL ok = ppid && Process32First(snap, &e); ok; ok = Process32Next(snap, &e))
        if (e.th32ProcessID == ppid) { snprintf(parent, sizeof parent, "%s", e.szExeFile); break; }
    CloseHandle(snap);
#elif defined(__linux__)
    char path[64], name[96] = "";
    snprintf(path, sizeof path, "/proc/%d/comm", (int)getppid());
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(name, sizeof name, f)) { name[strcspn(name, "\r\n")] = 0; snprintf(parent, sizeof parent, "%s", name); }
        fclose(f);
    }
#endif
}

void diagnostics_init(void) {
    start_ns = plat_now_ns();
    parent_name();
    {   // (the log's first line: which build, when, started by what - a launcher's log shows the same)
        time_t now = time(NULL);
        struct tm utc;
#ifdef _WIN32
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        char date[32];
        strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S UTC", &utc);
        fprintf(stderr, "[session] Super Odyssey PC Recomp %s (%s), %s, started by %s\n", APP_VERSION, SDL_GetPlatform(), date, parent);
    }
    // A small streaming fingerprint identifies private builds sharing a version
    // number. Cache it at startup; do not open the binary from a crash handler.
    char path[1024], bytes[4096];
    if (!plat_exe_path(path, sizeof path)) return;
    FILE *file = fopen(path, "rb");
    if (!file) return;
    uint64_t hash = 14695981039346656037ull, size = 0;
    size_t got;
    while ((got = fread(bytes, 1, sizeof bytes, file))) {
        size += got;
        for (size_t i = 0; i < got; i++) { hash ^= (unsigned char)bytes[i]; hash *= 1099511628211ull; }
    }
    if (!ferror(file)) snprintf(build, sizeof build, "fnv1a64:%016llx (%llu bytes)",
        (unsigned long long)hash, (unsigned long long)size);
    fclose(file);
}

void diagnostics_gpu(const char *description) {
    snprintf(gpu, sizeof gpu, "%s", description);
    atomic_store(&gpu_ready, true);
}

void diagnostics_fault(const char *what, uint64_t address, uint64_t pc, uint64_t lr) {
    if (atomic_flag_test_and_set(&fault_claimed)) return;
    snprintf(fault, sizeof fault, "%s, address %#llx, host PC %#llx, host LR %#llx",
             what, (unsigned long long)address, (unsigned long long)pc, (unsigned long long)lr);
    snprintf(fault_short, sizeof fault_short, "%s accessing %#llx", what, (unsigned long long)address);
    atomic_store(&fault_ready, true);
}

static void cpu_description(char text[128]) {
    snprintf(text, 128, "unavailable");
#if defined(__i386__) || defined(__x86_64__)
    if (__get_cpuid_max(0x80000000, NULL) >= 0x80000004) {
        unsigned words[12];
        for (unsigned i = 0; i < 3; i++)
            __get_cpuid(0x80000002 + i, &words[4*i], &words[4*i+1], &words[4*i+2], &words[4*i+3]);
        memcpy(text, words, sizeof words);
        text[sizeof words] = 0;
    }
#elif defined(__linux__)
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "model name", 10) && strncmp(line, "Hardware", 8)) continue;
            char *value = strchr(line, ':');
            if (value) { snprintf(text, 128, "%s", value + 1); break; }
        }
        fclose(f);
    }
#endif
    text[strcspn(text, "\r\n")] = 0;
    for (size_t n = strlen(text); n && text[n - 1] == ' '; n--) text[n - 1] = 0;  // (the CPU's own name is space-padded)
}

// Where the fault was: the recompiled game's code (f_<guest address>), a driver or other library (name.dll / .so), ours
static bool is_driver(const char *s) {
    return strstr(s, "nvoglv") || strstr(s, "nvidia") || strstr(s, "amdvlk") || strstr(s, "amdxx") || strstr(s, "radv") ||
           strstr(s, "igvk") || strstr(s, "vulkan") || strstr(s, "libvk");
}
static const char *place_of(bool located) {
    if (!located) return "unknown (no exception: see the log's last lines)";
    if (!strncmp(where_sym, "f_7", 3)) return "the game's own code (recompiled)";
    if (is_driver(where_sym)) return "the graphics driver";
    if (strstr(where_sym, ".dll") || strstr(where_sym, ".so")) return "a library (see Where)";
    return "the PC port's own code";
}

// ---- Why the game stopped, in plain words (2026-11-19: reports said only "No host exception recorded", and players
// could not tell running out of memory from a driver problem or a missing file)

// The log's last "FATAL: " line, or its last "*** " line (an exception, abort()) when that came later
static bool last_error_line(char *out, size_t n) {
    out[0] = 0;
    const char *log = plat_log_path();
    if (!log || !*log) return false;
    fflush(stderr);
    FILE *f = fopen(log, "rb");
    if (!f) return false;
    static char tail[16384];  // (static: a crash handler's stack may be small)
    fseek(f, 0, SEEK_END);
    long sz = ftell(f), from = sz > (long)sizeof tail - 1 ? sz - (long)sizeof tail + 1 : 0;
    fseek(f, from, SEEK_SET);
    size_t got = fread(tail, 1, sizeof tail - 1, f);
    tail[got] = 0;
    fclose(f);
    char *line = NULL, *star = NULL;
    for (char *p = tail; (p = strstr(p, "FATAL: ")) != NULL; p++) line = p;
    for (char *p = tail; (p = strstr(p, "*** ")) != NULL; p++) star = p;
    if (star && (!line || star > line)) line = star;
    if (!line) return false;
    size_t len = strcspn(line, "\r\n");
    snprintf(out, n, "%.*s", (int)(len < 400 ? len : 400), line);
    return true;
}

// The PC's free memory when it is nearly gone (a crash then is most likely an allocation that failed): MB left, or -1
static long long memory_low_mb(void) {
    if (getenv("ODYSSEY_TEST_MEMORY_LOW")) return 42;  // (tests: as if memory had run out)
#ifdef _WIN32
    MEMORYSTATUSEX m = {.dwLength = sizeof m};
    if (!GlobalMemoryStatusEx(&m)) return -1;
    // (the commit limit, RAM + page file: what an allocation needs on Windows)
    if (m.ullAvailPageFile < (512ull << 20) || m.ullAvailPageFile < m.ullTotalPageFile / 50) return (long long)(m.ullAvailPageFile >> 20);
#elif defined(__linux__)
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[128];
    unsigned long long avail = ~0ull, swap = 0, v;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "MemAvailable: %llu kB", &v) == 1) avail = v;
        else if (sscanf(line, "SwapFree: %llu kB", &v) == 1) swap = v;
    }
    fclose(f);
    if (avail != ~0ull && avail + swap < (256ull << 10)) return (long long)((avail + swap) >> 10);
#endif
    return -1;
}

static void cpu_features(char *out, size_t n) {
#if defined(__x86_64__)
    __builtin_cpu_init();
    snprintf(out, n, "AVX2 %s, FMA %s, BMI2 %s, AVX %s, SSE4.2 %s", __builtin_cpu_supports("avx2") ? "yes" : "no",
             __builtin_cpu_supports("fma") ? "yes" : "no", __builtin_cpu_supports("bmi2") ? "yes" : "no",
             __builtin_cpu_supports("avx") ? "yes" : "no", __builtin_cpu_supports("sse4.2") ? "yes" : "no");
#else
    snprintf(out, n, "(not x86-64)");
#endif
}

static bool has(const char *s, const char *a) { return strstr(s, a) != NULL; }

// crashed: false for a report made from the menu with nothing wrong
static void classify(bool crashed, char *head, size_t hn, char *advice, size_t an, char *detail, size_t dn) {
    char line[420];
    bool error = last_error_line(line, sizeof line), faulted = atomic_load(&fault_ready);
    if (!crashed && !faulted) error = false;  // (a menu report: an old FATAL line in the log isn't why)
    bool fatal = error && !strncmp(line, "FATAL: ", 7);
    const char *msg = fatal ? line + 7 : line;
    unsigned long code = 0;
    int sig = 0;
    if (faulted) {
        if (!strncmp(fault_short, "exception ", 10)) code = strtoul(fault_short + 10, NULL, 16);
        else if (!strncmp(fault_short, "signal ", 7)) sig = atoi(fault_short + 7);
    }
    long long low = memory_low_mb();
    snprintf(advice, an, "Attach this report to a bug report.");
    snprintf(detail, dn, "%s", error ? line : faulted ? fault_short : "none recorded (see the log's last lines)");
#ifdef _WIN32
    const char *driver = "Install the latest graphics driver (NVIDIA, AMD or Intel).";
#else
    const char *driver = "Update the graphics driver (your distribution's Mesa or NVIDIA driver).";
#endif
    int vk = 0;  // (a Vulkan call's error: "<call> failed: -N (file:line)", runtime/nvn/vulkan/vk.h VK_CHECK)
    const char *f = fatal ? strstr(msg, " failed: -") : NULL;
    if (f) vk = atoi(f + 9);
    const char *mem_advice = "Close other programs, set Performance > Memory use to Low, or let the system manage a larger "
                             "page file / swap.";
    if (fatal && (has(msg, "out of memory for a texture") || vk == -2 || has(msg, "OUT_OF_DEVICE_MEMORY"))) {
        snprintf(head, hn, "Out of graphics memory (VRAM)");
        snprintf(advice, an, "Lower the internal resolution (Graphics), set Performance > Memory use to Low and close other "
                 "programs. With integrated graphics, graphics memory is the PC's RAM and page file.");
    } else if (fatal && (vk == -1 || has(msg, "out of host memory") || has(msg, "out of memory") || has(msg, "cannot allocate"))) {
        snprintf(head, hn, "Out of memory (RAM)");
        snprintf(advice, an, "%s", mem_advice);
    } else if (fatal && (vk == -4 || has(msg, "device lost") || has(msg, "DEVICE_LOST"))) {
        snprintf(head, hn, "Vulkan device lost: the graphics card stopped responding");
        snprintf(advice, an, "%s Turn off GPU overclocks, lower the internal resolution, and try without the upscaler and "
                 "frame generation. If it happens in the same place every time, attach this report.", driver);
    } else if (fatal && (has(msg, "cannot open ") || has(msg, "cannot load ") || !strncmp(msg, "romfs: ", 7) ||
                         has(msg, "isn't in exefs") || has(msg, "no game code for") || has(msg, " is missing"))) {
        snprintf(head, hn, "A game file is missing or cannot be read");
        snprintf(advice, an, "The file named below could not be read. Check that the game's folder (extracted/: exefs and "
                 "romfs) is complete and its drive is connected, or run the setup again with your game files.");
    } else if (fatal && (vk || has(msg, "Vulkan") || has(msg, "VK_"))) {
        if (vk) snprintf(head, hn, "Graphics driver error (Vulkan error %d)", vk);
        else snprintf(head, hn, "Graphics driver error (Vulkan)");
        snprintf(advice, an, "%s The game needs Vulkan 1.3.", driver);
    } else if (code == 0xc000001d || code == 0xc0000096 || sig == 4) {
        char cpu[128], feats[128];
        cpu_description(cpu);
        cpu_features(feats, sizeof feats);
        snprintf(head, hn, "Illegal instruction: the processor lacks an instruction the game uses (CPU features)");
        snprintf(advice, an, "This build may need a newer processor than this one (%s). Attach this report: it names the "
                 "processor and its features.", cpu);
        snprintf(detail, dn, "%s; CPU features: %s", faulted ? fault_short : line, feats);
    } else if (fatal) {
        size_t len = strcspn(msg, "\r\n");
        while (len && (msg[len - 1] == ' ' || msg[len - 1] == '.')) len--;
        snprintf(head, hn, "Error: %.*s", (int)(len < 140 ? len : 140), msg);
    } else if (code == 0xc0000017 || ((faulted || error) && low >= 0)) {
        snprintf(head, hn, "Out of memory (RAM): the game crashed with %lld MB of memory left", low < 0 ? 0 : low);
        snprintf(advice, an, "%s", mem_advice);
    } else if (code == 0xc00000fd) {
        snprintf(head, hn, "Stack overflow");
    } else if (code == 0xc0000374) {
        snprintf(head, hn, "Memory corruption (heap)");
    } else if (code == 0xc0000409 || sig == 6 || (error && has(line, "abort()"))) {
        snprintf(head, hn, "The program aborted");
    } else if (faulted || error) {
        bool located = atomic_load(&where_ready);
        const char *kind = code == 0xc0000005 || sig == 11 ? " (access violation)" : sig == 7 ? " (bus error)" :
                           sig == 8 || code == 0xc0000094 ? " (arithmetic error)" : "";
        if (located && is_driver(where_sym)) {
            snprintf(head, hn, "A crash in the graphics driver%s", kind);
            snprintf(advice, an, "%s Attach this report if it keeps happening.", driver);
        } else if (located) snprintf(head, hn, "A crash%s in %s", kind, place_of(true));
        else snprintf(head, hn, "A crash%s", kind);
    } else if (crashed) {
        snprintf(head, hn, "The game stopped without an error message");
        snprintf(advice, an, "Attach this report: the log's last lines show what the game was doing.");
    } else {
        snprintf(head, hn, "No crash: a report made from the menu");
        snprintf(advice, an, "Attach it to the bug report it belongs to.");
    }
}

void diagnostics_reason(char *head, size_t hn, char *advice, size_t an, char *detail, size_t dn) {
    classify(true, head, hn, advice, an, detail, dn);
}

void diagnostics_fatal_text(char *text, size_t capacity) {
    char head[256], advice[512], detail[480];
    classify(true, head, sizeof head, advice, sizeof advice, detail, sizeof detail);
    snprintf(text, capacity, "Super Odyssey PC Recomp stopped.\n\nReason: %s\n\n%s\n\nError: %s", head, advice, detail);
}

// Preserve startup hardware information and the most recent fault/call chain,
// while bounding a verbose log. Never enumerate directories or attach assets.
static void append_file(FILE *out, const char *title, const char *path, long limit, bool log) {
    fprintf(out, "\n--- %s ---\n", title);
    if (!path || !*path) { fprintf(out, "Unavailable (output may be in a terminal).\n"); return; }
    fprintf(out, "Path: %s\n", path);
    FILE *in = fopen(path, "rb");
    if (!in) { fprintf(out, "Unavailable: %s\n", strerror(errno)); return; }
    if (fseek(in, 0, SEEK_END) || ftell(in) < 0) { fclose(in); fprintf(out, "Cannot read file length.\n"); return; }
    long size = ftell(in), head = size > limit && log ? 65536 : 0;
    fseek(in, 0, SEEK_SET);
    char bytes[4096];
    long left = head ? head : size > limit ? limit : size;
    for (;;) {
        while (left > 0) {
            size_t got = fread(bytes, 1, left < (long)sizeof bytes ? (size_t)left : sizeof bytes, in);
            if (!got) break;
            fwrite(bytes, 1, got, out);
            left -= (long)got;
        }
        if (!head) break;
        fprintf(out, "\n[Middle of log omitted; original file is %ld bytes.]\n", size);
        head = 0;
        fseek(in, size - limit, SEEK_SET);
        left = limit;
    }
    if (size > limit && !log) fprintf(out, "\n[Settings truncated at %ld bytes.]\n", limit);
    fclose(in);
}

static bool report_directory(char directory[1024]) {
    char fallback[1024];
    const char *root = NULL, *log = plat_log_path();
    if (log && *log) snprintf(fallback, sizeof fallback, "%s", log);
    else if (rt_data_root() && *rt_data_root()) root = rt_data_root();
    else if (!plat_exe_path(fallback, sizeof fallback)) return false;
    if (!root || !*root) {
        char *a = strrchr(fallback, '/'), *b = strrchr(fallback, '\\');
        char *last = !a ? b : !b ? a : a > b ? a : b;
        if (!last) return false;
        *last = 0;
        root = fallback;
    }
    return snprintf(directory, 1024, "%s/reports", root) < 1024;
}

static bool write_report(char *path, size_t capacity, bool crash) {
    if (!capacity) return false;
    path[0] = 0;
    char directory[1024];
    if (!report_directory(directory)) return false;
    if (mkdir(directory, 0755) && errno != EEXIST) return false;
    time_t now = time(NULL);
    struct tm utc;
#ifdef _WIN32
    gmtime_s(&utc, &now);
    unsigned pid = (unsigned)_getpid();
#else
    gmtime_r(&now, &utc);
    unsigned pid = (unsigned)getpid();
#endif
    char date[32];
    strftime(date, sizeof date, "%Y%m%d-%H%M%S", &utc);
    int len = snprintf(path, capacity, "%s/report-%s-%u-%u.txt", directory, date, pid, atomic_fetch_add(&sequence, 1));
    if (len < 0 || (size_t)len >= capacity) { path[0] = 0; return false; }
    int flags = O_CREAT | O_EXCL | O_WRONLY;
#ifdef _WIN32
    flags |= O_BINARY;
#endif
    int fd = open(path, flags, 0600);
    if (fd < 0) { path[0] = 0; return false; }
    FILE *out = fdopen(fd, "wb");
    if (!out) { close(fd); remove(path); path[0] = 0; return false; }
    char cpu[128];
    cpu_description(cpu);
    char os[128];
    snprintf(os, sizeof os, "%s", SDL_GetPlatform());
#ifdef _WIN32
    LONG (WINAPI *version)(OSVERSIONINFOW *) = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v = {.dwOSVersionInfoSize = sizeof v};
    if (version && !version(&v)) snprintf(os, sizeof os, "Windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
#endif
    // ---- Summary: what a player or a developer reads first (format 2, 2026-11-17; 3: the reason first, 2026-11-19)
    extern const char *odyssey_stage_name(void);
    extern uint64_t renderer_present_count(void);
    Module *main = loader_module("main");
    char build_id[40] = "not loaded", game[48] = "not loaded";
    if (main) {
        for (unsigned i = 0; i < 8 && i < sizeof main->build_id; i++) snprintf(build_id + 2 * i, 3, "%02x", main->build_id[i]);
        snprintf(game, sizeof game, "%s", !strcmp(build_id, "3ca12dfaaf9c82da") ? "1.0.0" :
                 !strcmp(build_id, "b424be150a8e7d78") ? "1.4.1" : "another version");
    }
    bool faulted = atomic_load(&fault_ready), located = atomic_load(&where_ready);
    const char *place = place_of(located);
    // The first lines: why, in plain words (a player sees these in the issue before anything else)
    char head[256], advice[512], detail[480], feats[128];
    classify(crash, head, sizeof head, advice, sizeof advice, detail, sizeof detail);
    cpu_features(feats, sizeof feats);
    fprintf(out, "Crash reason: %s\nWhat to try: %s\nError: %s\n\n", head, advice, detail);
    static const char *const tech[TEMPORAL_BACKEND_COUNT] = {"NVIDIA DLSS", "AMD FSR 3.1", "AMD FSR 4", "Intel XeSS", "AMD FSR 2"};
    static const char *const modes[DLSS_MODE_COUNT] = {"off", "native AA", "quality", "balanced", "performance"};
    int tb = g_settings.temporal_backend, tm = *settings_temporal_mode();
    char upscaler[64];
    if (g_settings.temporal_off || tb < 0 || tb >= TEMPORAL_BACKEND_COUNT) snprintf(upscaler, sizeof upscaler, "off");
    else snprintf(upscaler, sizeof upscaler, "%s %s", tech[tb], tm >= 0 && tm < DLSS_MODE_COUNT ? modes[tm] : "?");
    uint64_t secs = start_ns ? (plat_now_ns() - start_ns) / 1000000000ull : 0;
    char signature[256];  // (the same crash from two players gives the same line: what, where, stage)
    snprintf(signature, sizeof signature, "%s | %s+%llx | %s | game %s", faulted ? fault_short : head,
             located ? where_sym : "?", (unsigned long long)(located ? where_off : 0), odyssey_stage_name(), game);
    fprintf(out, "Super Odyssey PC Recomp %s %s (format 3)\n\n"
                 "=== Summary ===\n"
                 "What happened: %s\n"
                 "Where: %s (%s+%#llx)\n"
                 "Last stage: %s, frame %llu, %llu min %llu s into the session\n"
                 "Frame rate: setting %d, drawing %d a second; upscaler %s; frame generation %s\n"
                 "Memory: %llu MB in RAM, %llu MB committed, of %llu MB RAM\n"
                 "Game: Super Mario Odyssey %s (build %s); started by %s\n"
                 "Signature: %s\n\n",
            APP_VERSION, crash ? "crash report" : "report", faulted ? fault_short : crash ? "no processor exception (the reason "
            "above is the log's last error line)" : "nothing (a report made from the menu)", place,
            located ? where_sym : "?", (unsigned long long)(located ? where_off : 0), odyssey_stage_name(),
            (unsigned long long)renderer_present_count(), (unsigned long long)(secs / 60), (unsigned long long)(secs % 60),
            settings_game_hz(), odyssey_native_hz(), upscaler,
            g_settings.frame_gen == FG_FSR ? "AMD FSR" : g_settings.frame_gen == FG_DLSS ? "NVIDIA DLSS" : "off",
            (unsigned long long)(plat_resident_bytes() >> 20), (unsigned long long)(plat_private_bytes() >> 20),
            (unsigned long long)(plat_total_ram() >> 20), game, build_id, parent, signature);
    fprintf(out, "=== This PC ===\nApp: %s\nExecutable fingerprint: %s\nUTC: %s\nOS: %s\n"
                 "CPU: %s (%s)\nLogical processors: %d\nRAM: %llu MiB\nGPU / driver: %s\n"
                 "Fault: %s\n", APP_VERSION, build, date, os, cpu, feats, SDL_GetNumLogicalCPUCores(),
                 (unsigned long long)(plat_total_ram() >> 20), atomic_load(&gpu_ready) ? gpu : "unavailable (see log)",
                 faulted ? fault : "no processor exception (see Crash reason at the top, and the log's last lines)");
    if (main) {
        fprintf(out, "Game main build ID: ");
        for (unsigned i = 0; i < sizeof main->build_id; i++) fprintf(out, "%02x", main->build_id[i]);
        fprintf(out, "\n");
    } else fprintf(out, "Game main build ID: not loaded\n");
    fprintf(out, "Contains saved settings and a bounded log, copied as-is and including local paths. No process memory dump or additional game/key/save files are collected.\n");
    fflush(stderr);
    append_file(out, "Saved settings", settings_current_file(), 65536, false);
    append_file(out, "Log (startup and most recent entries)", plat_log_path(), 1024 * 1024, true);
    bool ok = !ferror(out);
    if (fclose(out)) ok = false;
    if (!ok) { remove(path); path[0] = 0; }
    return ok;
}

bool diagnostics_write_report(char *path, size_t capacity) { return write_report(path, capacity, false); }

// The last crash, remembered for the next start: reports/.last-crash holds the report's name, its reason, and "seen"
// once the next start's notice has shown it. Hidden, and not a .txt (issue #75: a player attached the old
// last-crash.txt - only the report's name - instead of the report). Older builds wrote last-crash.txt: still read.
static bool marker_path(char *marker, size_t n, bool legacy) {
    char directory[1024];
    if (!report_directory(directory)) return false;
    return snprintf(marker, n, "%s/%s", directory, legacy ? "last-crash.txt" : ".last-crash") < (int)n;
}

static void marker_write(const char *name, const char *reason, bool seen) {
    char marker[1100], legacy[1100];
    if (!marker_path(marker, sizeof marker, false)) return;
#ifdef _WIN32
    SetFileAttributesA(marker, FILE_ATTRIBUTE_NORMAL);  // (a hidden file can't be replaced by fopen "wb")
#endif
    FILE *out = fopen(marker, "wb");
    if (!out) return;
    fprintf(out, "%s\n%s\n%s", name, reason, seen ? "seen\n" : "");
    fclose(out);
#ifdef _WIN32
    SetFileAttributesA(marker, FILE_ATTRIBUTE_HIDDEN);
#endif
    if (marker_path(legacy, sizeof legacy, true)) remove(legacy);
}

bool diagnostics_capture_crash(char *path, size_t capacity) {
    if (!write_report(path, capacity, true)) return false;
    char head[256], advice[8], detail[8];
    classify(true, head, sizeof head, advice, sizeof advice, detail, sizeof detail);
    const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
    marker_write((a > b ? a : b) + 1, head, false);
    return true;
}

bool diagnostics_last_crash_info(char *path, size_t capacity, char *reason, size_t rn, bool *seen) {
    if (reason && rn) reason[0] = 0;
    if (seen) *seen = true;
    if (!capacity) return false;
    path[0] = 0;
    char directory[1024], marker[1100], name[160], why[256] = "", flag[16] = "";
    if (!report_directory(directory)) return false;
    bool legacy = false;
    FILE *in = marker_path(marker, sizeof marker, false) ? fopen(marker, "rb") : NULL;
    if (!in && marker_path(marker, sizeof marker, true)) in = fopen(marker, "rb"), legacy = true;
    if (!in) return false;
    bool read = fgets(name, sizeof name, in) != NULL;
    if (read && !legacy && fgets(why, sizeof why, in)) { if (!fgets(flag, sizeof flag, in)) flag[0] = 0; }
    fclose(in);
    if (!read) return false;
    name[strcspn(name, "\r\n")] = 0;
    why[strcspn(why, "\r\n")] = 0;
    size_t n = strlen(name);
    if (n < 11 || strncmp(name, "report-", 7) || strcmp(name + n - 4, ".txt") || strpbrk(name, "/\\:")) return false;
    int len = snprintf(path, capacity, "%s/%s", directory, name);
    struct stat st;
    if (len < 0 || (size_t)len >= capacity || stat(path, &st) || !S_ISREG(st.st_mode)) { path[0] = 0; return false; }
    if (reason && rn) snprintf(reason, rn, "%s", why[0] ? why : "unknown (a report from an older version)");
    if (seen) *seen = legacy || !strncmp(flag, "seen", 4);  // (an older version's crash: no notice for it)
    return true;
}

bool diagnostics_last_crash(char *path, size_t capacity) {
    return diagnostics_last_crash_info(path, capacity, NULL, 0, NULL);
}

void diagnostics_last_crash_seen(void) {
    char path[1200], reason[256];
    bool seen;
    if (!diagnostics_last_crash_info(path, sizeof path, reason, sizeof reason, &seen) || seen) return;
    const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
    marker_write((a > b ? a : b) + 1, reason, true);
}

// ---- Attaching a report

// The report's text for pasting into an issue: GitHub takes 65,536 characters in a comment, so a long report keeps its
// first part (reason, summary, PC, settings) and the log's last lines. Markdown: the reason visible, the rest folded.
static char *clip_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0 || size > (64l << 20)) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1);
    if (!text) { fclose(f); return NULL; }
    size_t got = fread(text, 1, (size_t)size, f);
    fclose(f);
    text[got] = 0;
    for (size_t i = 0; i < got; i++) if (!text[i]) text[i] = ' ';
    const char *a = strrchr(path, '/'), *b = strrchr(path, '\\'), *name = (a > b ? a : b) ? (a > b ? a : b) + 1 : path;
    char reason[300] = "";
    if (!strncmp(text, "Crash reason: ", 14)) snprintf(reason, sizeof reason, "%.*s", (int)strcspn(text + 14, "\r\n"), text + 14);
    enum { BUDGET = 60000, HEAD = 20000 };
    size_t head = got, tail_from = got;
    const char *log = strstr(text, "\n--- Log");
    if (got > BUDGET) {
        head = log ? (size_t)(log - text) + 1 : HEAD;
        if (head > HEAD) head = HEAD;
        while (head && text[head - 1] != '\n') head--;
        tail_from = got - (BUDGET - head);
        while (tail_from < got && text[tail_from - 1] != '\n') tail_from++;
    }
    size_t cap = head + (got - tail_from) + 1024;
    char *out = malloc(cap);
    if (!out) { free(text); return NULL; }
    int n = snprintf(out, cap, "**Crash report:** %s\n\n<details><summary>%s</summary>\n\n````text\n",
                     reason[0] ? reason : "(see below)", name);
    memcpy(out + n, text, head);
    n += (int)head;
    if (tail_from < got) {
        n += snprintf(out + n, cap - n, "\n[... trimmed to fit a comment: the file %s has all of it ...]\n\n", name);
        memcpy(out + n, text + tail_from, got - tail_from);
        n += (int)(got - tail_from);
    }
    snprintf(out + n, cap - n, "\n````\n\n</details>\n");
    free(text);
    return out;
}

#ifndef _WIN32
typedef struct { char *text; unsigned id; } ClipboardRequest;
static atomic_uint copy_sequence;
static void SDLCALL copy_on_event_thread(void *data) {
    ClipboardRequest *request = data;
    if (atomic_load(&copy_sequence) == request->id) {
        bool copied = SDL_SetClipboardText(request->text);
        if (atomic_load(&copy_sequence) == request->id) atomic_store(&copy_status, copied ? 1 : -1);
    }
    free(request->text);
    SDL_free(request);
}
#endif

// text: malloc'd UTF-8, taken over
static bool copy_text(char *text) {
    atomic_store(&copy_status, -1);
#ifdef _WIN32
    // (the log is UTF-8; a path in it may be in the ANSI code page: then the whole text as ANSI)
    UINT cp = CP_UTF8;
    int count = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!count) cp = CP_ACP, count = MultiByteToWideChar(cp, 0, text, -1, NULL, 0);
    if (!count) { free(text); return false; }
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (size_t)count * sizeof(wchar_t));
    if (!data) { free(text); return false; }
    wchar_t *wide = GlobalLock(data);
    if (!wide) { GlobalFree(data); free(text); return false; }
    MultiByteToWideChar(cp, 0, text, -1, wide, count);
    GlobalUnlock(data);
    free(text);
    bool ok = false;
    // EmptyClipboard needs a real owner for SetClipboardData. A message-only
    // window also works before SDL creates its game window after an early fault.
    HWND owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    if (owner && OpenClipboard(owner)) {
        if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, data)) ok = true;
        CloseClipboard();
    }
    if (owner) DestroyWindow(owner);
    if (!ok) GlobalFree(data);
    atomic_store(&copy_status, ok ? 1 : -1);
    return ok;
#else
    // SDL clipboard APIs are main-thread-only (called from the main thread, SDL runs it at once). Do not wait while
    // the renderer owns its lock: the event thread might itself be waiting for rendering.
    ClipboardRequest *request = SDL_malloc(sizeof *request);
    if (!request) { free(text); return false; }
    request->text = text;
    request->id = atomic_fetch_add(&copy_sequence, 1) + 1;
    atomic_store(&copy_status, 0);
    if (SDL_RunOnMainThread(copy_on_event_thread, request, false)) return true;
    free(request->text); SDL_free(request);
    atomic_store(&copy_status, -1);
    return false;
#endif
}

bool diagnostics_copy_report(const char *path) {
    char *text = clip_text(path);
    if (!text) { atomic_store(&copy_status, -1); return false; }
    return copy_text(text);
}

// The report in the file manager: Explorer with the file selected; elsewhere its folder (file managers differ in
// how to select a file)
bool diagnostics_show_report(const char *path) {
#ifdef _WIN32
    char native[1200], command[1300];
    snprintf(native, sizeof native, "%s", path);
    for (char *p = native; *p; p++) if (*p == '/') *p = '\\';
    snprintf(command, sizeof command, "explorer.exe /select,\"%s\"", native);
    STARTUPINFOA si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    char url[3700] = "file://";
    size_t n = strlen(url);
    const char *a = strrchr(path, '/');
    size_t len = a ? (size_t)(a - path) : strlen(path);
    for (size_t i = 0; i < len && n + 4 < sizeof url; i++) {
        unsigned char c = (unsigned char)path[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("/-_.~", c)) url[n++] = (char)c;
        else n += (size_t)snprintf(url + n, sizeof url - n, "%%%02X", c);
    }
    url[n] = 0;
    return SDL_OpenURL(url);
#endif
}

void diagnostics_fatal_dialog(const char *message) {
    // Save before displaying the dialog: closing it or restarting the game must
    // not erase the only fault log. Export remains best-effort after corruption.
    char path[1200];
    bool saved = diagnostics_capture_crash(path, sizeof path);
    static char text[4096];
    snprintf(text, sizeof text, "%s\n\n%s", message, saved ?
             "A crash report was saved. To report this: Copy report text, then paste it into the issue - or Show report "
             "file and attach that file." : "No crash report could be saved: attach the log named above.");
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Close"},
        {0, 1, "Copy report text"},
        {0, 2, "Show report file"}};
    SDL_MessageBoxData box = {.flags = SDL_MESSAGEBOX_ERROR, .title = "Super Odyssey PC Recomp",
        .message = text, .numbuttons = saved ? 3 : 1, .buttons = buttons};
    // (ODYSSEY_TEST_CRASH_DIALOG=copy / show / close: tests press that button without showing the box)
    const char *test = getenv("ODYSSEY_TEST_CRASH_DIALOG");
    for (int round = 0; round < 8; round++) {
        int selected = 0;
        if (test && *test) {
            selected = !strcmp(test, "copy") ? 1 : !strcmp(test, "show") ? 2 : 0;
            fprintf(stderr, "[report] dialog (test): %s\n", text);
            if (round) selected = 0;
        } else if (!SDL_ShowMessageBox(&box, &selected)) {
            fprintf(stderr, "[report] message box unavailable: %s\n", SDL_GetError());
#ifdef _WIN32
            MessageBoxA(NULL, text, box.title, MB_ICONERROR | MB_OK);
#else
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, box.title, text, NULL);
#endif
            return;
        }
        if (selected != 1 && selected != 2) return;
        bool done = selected == 1 ? diagnostics_copy_report(path) && diagnostics_copy_status() == 1 : diagnostics_show_report(path);
        fprintf(stderr, "[report] %s: %s\n", selected == 1 ? "copy report text" : "show report file", done ? "done" : "failed");
        snprintf(text, sizeof text, "%s\n\n%s\n%s", message,
                 selected == 1 ? (done ? "The report's text is copied. Paste it into the bug report (Ctrl+V)." :
                                         "The text could not be copied. Use Show report file and attach the file:")
                               : (done ? "The report file is selected in its folder: attach it to the bug report." :
                                         "The folder could not be opened. The report file is:"),
                 path);
    }
}
