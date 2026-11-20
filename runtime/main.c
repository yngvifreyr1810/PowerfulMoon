// Super Odyssey PC Recomp entry: load modules, set up the kernel, run nn::init::Start on the main thread.
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "gamelib.h"
#include "diagnostics.h"
#include "paths.h"
#include "settings.h"
#include "hle.h"
#include "kernel.h"
#include "rt.h"
#include "romfs_build.h"
#include "ui/setup.h"
#include "updater.h"

const char *g_romfs_dir;

// rtld normally passes these to nn::init::Start: run every module's init/fini arrays.
static void run_array(Cpu *c, uint64_t arr, uint64_t size) {
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t fn = ((uint64_t *)arr)[i];
        if (fn) guest_call(c, fn);
    }
}

// nn::init::Start calls x2 right after nnosInitialize and x3 after nninitStartup, just before
// nnMain: SDK constructors run early, the game's run once nninitStartup has set up its heap.
// rtld runs a module's DT_INIT, which registers it (and its TLS template) with musl via
// __nnmusl_init_dso and then runs its init_array; fall back to the array if there is no DT_INIT.
static void module_init(Cpu *c, Module *m) {
    LOG("init %s (DT_INIT %llx, %llu init_array entries)\n", m->name, m->init, m->init_array_sz / 8);
    if (m->init) guest_call(c, m->init);
    else run_array(c, m->init_array, m->init_array_sz);
}

// subsdk0 (Odyssey's multimedia library) goes with the game: its constructors may allocate, and main depends on it.
static void rtld_init_sdk(Cpu *c) { module_init(c, loader_module("sdk")); }
static void rtld_init_main(Cpu *c) {
    Module *sub = loader_module("subsdk0");
    if (sub) module_init(c, sub);
    module_init(c, loader_module("main"));
}

// Claim the guest address range before anything else can map there. The host allocator places
// regions at random addresses in the same window; on a collision, re-exec for a fresh layout.
__attribute__((constructor(101))) static void reserve_guest_space(void) {
    if (plat_reserve_guest(GUEST_LO, GUEST_HI)) return;
    const char *tries = getenv("ODYSSEY_REEXEC");
    int n = tries ? atoi(tries) : 0;
    if (n >= 20) {
        fprintf(stderr, "FATAL: guest address range %llx-%llx is not free\n", GUEST_LO, GUEST_HI);
        exit(1);
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    setenv("ODYSSEY_REEXEC", buf, 1);
    plat_exec(NULL);
    perror("re-exec");
    exit(1);
}

// ------------------------------------------------------------------ boot / in-place reboot
// Restarting the game (menu) reboots it inside this process: the window, GPU device, settings and shader caches stay.
// rt_request_reboot (menu, any thread) makes ui_run return on the main thread, which then: stops every guest thread
// (kernel_stop_threads: each unwinds at its next svc / HLE call / wait), resets the renderer (textures, address-keyed
// caches) and all per-boot host state (RT_ON_RESET hooks: services, HLE features, audio), empties the kernel, wipes
// guest memory, and boots again.
static _Atomic int g_reboot;
void rt_request_reboot(int kind) {
    LOG("[reboot] requested\n");
    g_reboot = kind;
}
int rt_reboot_requested(void) { return g_reboot; }

// ODYSSEY_REBOOT_AT=<frame>[,...] (test): restart the game in place at that presented frame (frames count on).
// Called by the presenter every frame.
void reboot_test_frame(uint64_t frame) {
    static const char *next;
    static int init;
    if (!init) { init = 1; next = getenv("ODYSSEY_REBOOT_AT"); }
    if (!next || !*next || (long)frame < atol(next)) return;
    next = strchr(next, ',');
    if (next) next++;
    rt_request_reboot(RT_REBOOT_GAME);
}

#ifdef __APPLE__
extern const ResetFn __start_resets[] __asm__("section$start$__DATA$__rt_reset");
extern const ResetFn __stop_resets[] __asm__("section$end$__DATA$__rt_reset");
#elif defined(__linux__)
extern const ResetFn __start_resets[], __stop_resets[];  // (ELF: defined by the linker)
#else
// PE/COFF: .rst$a < .rst$m (entries, hle.h) < .rst$z (the linker sorts by the text after '$')
__attribute__((section(".rst$a"), used)) static const ResetFn __start_resets[1];
__attribute__((section(".rst$z"), used)) static const ResetFn __stop_resets[1];
#endif

void backend_reset(void);             // renderer: GPU idle, game textures and address-keyed caches dropped
void backend_guest_memory_reset(void);  // renderer: guest memory was replaced (re-wrap the heap)
void backend_clear_window(void);      // a black frame while the game is down

static void reboot_idle(void) {  // (main thread, while guest threads stop) keep the window alive
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT) { LOG("window closed\n"); extern void vk_exit_now(void); vk_exit_now(); }
}

static void teardown(void) {
    uint64_t t0 = plat_now_ns();
    kernel_stop_threads(reboot_idle);
    backend_clear_window();
    backend_reset();
    const ResetFn *start = __start_resets, *stop = __stop_resets;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    int n = 0;
    for (const ResetFn *p = start; p < stop; p++)
        if (*p) { (*p)(); n++; }
    kernel_reset();
    if (!plat_reset_guest(GUEST_LO, GUEST_HI)) FATAL("cannot reset guest memory\n");
    backend_guest_memory_reset();
    g_nmodules = 0;
    g_reboot = RT_REBOOT_NONE;
    LOG("[reboot] game torn down in %llums (%d reset hooks)\n", (unsigned long long)((plat_now_ns() - t0) / 1000000), n);
}

static void boot(const char *root) {
    char exefs[1024], romfs[1024], npdm_path[1024];
    snprintf(exefs, sizeof exefs, "%s/exefs", root);
    snprintf(romfs, sizeof romfs, "%s/romfs", root);
    snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs);
    if (!g_romfs_dir) g_romfs_dir = strdup(romfs);

    char why[512];
    if (!gamelib_load(root, why, sizeof why)) FATAL("%s\n", why);
    loader_load(exefs);
    { extern void settings_load(void); settings_load(); }
    kernel_init();
    dispatch_init();
    if (game_is_odyssey()) {
        odyssey_boot();
        odyssey_native_120_boot();
        odyssey_coop_boot();
    }

    uint32_t stack_size = 0x100000;
    int prio = 44;
    FILE *f = fopen(npdm_path, "rb");
    if (f) {
        uint8_t meta[0x80];
        if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4)) {
            prio = meta[0xE];
            stack_size = *(uint32_t *)(meta + 0x1C);
        }
        fclose(f);
    }
    LOG("main thread: prio %d, stack 0x%x\n", prio, stack_size);

    uint64_t start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (!start) FATAL("nn::init::Start not found\n");
    uint64_t init_fn = hle_register("<rtld_init_sdk>", rtld_init_sdk);
    uint64_t fini_fn = hle_register("<rtld_init_main>", rtld_init_main);
    KThread *t = kernel_create_main_thread(start, stack_size, prio);
    kernel_run_main(t, t->handle, 0, init_fn, fini_fn);  // nn::init::Start(thread_handle, args_region, init, fini)
}

// (tests) ODYSSEY_MEM_TRACE=1: memory in RAM / committed every half second from the start, in the log - each jump next
// to the log lines around it shows which step of the start-up it came from
static int mem_trace(void *arg) {
    (void)arg;
    uint64_t t0 = plat_now_ns(), last = 0;
    for (;;) {
        uint64_t c = plat_private_bytes();
        LOG("[memtrace] %.1f s: %llu MB in RAM, %llu MB committed\n", (plat_now_ns() - t0) / 1e9,
            (unsigned long long)(plat_resident_bytes() >> 20), (unsigned long long)(c >> 20));
#ifdef _WIN32
        extern void plat_mem_map_log(void);  // (what it is made of, each time it moved by 150 MB)
        if (c > last + (150ull << 20) || c + (150ull << 20) < last) { plat_mem_map_log(); last = c; }
#endif
        SDL_Delay(500);
    }
    return 0;
}

// ODYSSEY_TEST_CRASH=<kind> (tests of the crash report and its notices, 2026-11-19): ODYSSEY_TEST_CRASH_AFTER seconds
// (default 20) into the game, stop as that kind of crash does - the same error lines as the real ones. vram: a Vulkan
// call out of device memory; ram: out of host memory; devlost: Vulkan device lost; illegal: an illegal instruction;
// fatal: another FATAL error; segv: an access violation; ram-segv: an access violation with memory reported low
// (ODYSSEY_TEST_MEMORY_LOW). missing-file is in services/fs.c: the RomFS image opened under a name that isn't there.
static int test_crash(void *arg) {
    const char *kind = arg, *after = getenv("ODYSSEY_TEST_CRASH_AFTER");
    SDL_Delay((Uint32)((after ? atof(after) : 20) * 1000));
    LOG("[test] crash: %s\n", kind);
    if (!strcmp(kind, "vram")) FATAL("vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL) failed: %d (%s:%d)\n", -2, __FILE__, __LINE__);
    if (!strcmp(kind, "ram")) FATAL("nvn: out of host memory for commands\n");
    if (!strcmp(kind, "devlost")) FATAL("vkQueueSubmit(g_vk.queue, 1, &si, fence) failed: %d (%s:%d)\n", -4, __FILE__, __LINE__);
    if (!strcmp(kind, "illegal")) __builtin_trap();
    if (!strcmp(kind, "fatal")) FATAL("test: the game asked for something the PC port does not have yet\n");
    if (!strcmp(kind, "ram-segv")) {
#ifdef _WIN32
        _putenv_s("ODYSSEY_TEST_MEMORY_LOW", "1");
#else
        setenv("ODYSSEY_TEST_MEMORY_LOW", "1", 1);
#endif
    }
    *(volatile int *)(uintptr_t)8 = 1;
    return 0;
}

int main(int argc, char **argv) {
    plat_init();
    diagnostics_init();
    if (getenv("ODYSSEY_MEM_TRACE")) SDL_DetachThread(SDL_CreateThread(mem_trace, "memtrace", NULL));
#ifdef _WIN32
    setvbuf(stderr, NULL, _IONBF, 0);  // the Windows CRT treats _IOLBF as full buffering
#else
    setvbuf(stderr, NULL, _IOLBF, 0);
#endif
    { extern void input_steam_motion_prepare(void); input_steam_motion_prepare(); }  // (before SDL sees controllers)
#ifdef __linux__
    // Setup's shader pass compiles every program in the game files once into the driver's own disk cache (~650 MB on
    // NVIDIA, 2026-11-19), from which the game's first draws take them in ~0.1 ms; Mesa's default limit is 1 GB, shared
    // with other programs. (Under Steam, Steam's own setting is kept.)
    setenv("MESA_SHADER_CACHE_MAX_SIZE", "4G", 0);
#endif
    const char *root = "extracted";  // (in the data root)
    bool build_romfs = false;
    const char *prepare = NULL;
    const char *unpack_seed = NULL;  // --unpack-seed <out>: the shipped pipeline seed, unpacked there
    static char arg_root[1024];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--play")) {  // straight into the game, without the configuration page
#ifdef _WIN32
            _putenv_s("ODYSSEY_NO_CONFIG", "1");
#else
            setenv("ODYSSEY_NO_CONFIG", "1", 1);
#endif
            continue;
        }
        else if (!strcmp(argv[i], "--build-romfs")) build_romfs = true;
        else if (!strcmp(argv[i], "--unpack-seed") && i + 1 < argc) unpack_seed = argv[++i];  // (tools/release.sh)
        else if (!strcmp(argv[i], "--prepare-shaders") && i + 1 < argc) prepare = argv[++i];
        else if (!strcmp(argv[i], "--data-root") && i + 1 < argc) {  // (the game's own, for its shader preparation)
#ifdef _WIN32
            _putenv_s("ODYSSEY_DATA_ROOT", argv[++i]);
#else
            setenv("ODYSSEY_DATA_ROOT", argv[++i], 1);
#endif
        }
        else if (argv[i][0] != '-' && !realpath(argv[i], arg_root)) root = argv[i];  // (relative to where we started)
        else if (argv[i][0] != '-') root = arg_root;
    }
    rt_paths_init();  // the data root becomes the working directory (runtime/paths.h)
#ifndef __APPLE__
    // --prepare-shaders <list>: the shader preparation's own process, started by the game (runtime/nvn/vulkan/exec.c)
#ifndef __APPLE__
    if (unpack_seed) {  // (after rt_paths_init: the seed is found next to the executable)
        extern bool vk_seed_unpack(const char *src, const char *dst);
        char seed[1100];
        bool ok = rt_resource("pipeline-seed.bin.zst", "data/pipeline-seed.bin.zst", seed, sizeof seed) &&
                  vk_seed_unpack(seed, unpack_seed);
        LOG("pipeline seed %s\n", ok ? "unpacked" : "not unpacked");
        return ok ? 0 : 1;
    }
#else
    (void)unpack_seed;
#endif
    if (prepare) {
        extern int vk_prepare_main(const char *list);
        return vk_prepare_main(prepare);
    }
#else
    (void)prepare;
#endif
    updater_startup();  // (after an update: the old process gone, its old files removed)
    if (build_romfs) {  // --build-romfs: <root>/romfs.bin from <root>/romfs, as the start screen's setup does
        char dir[1100], out[1100], why[256];
        snprintf(dir, sizeof dir, "%s/romfs", root);
        snprintf(out, sizeof out, "%s/romfs.bin", root);
        LOG("building %s from %s (%llu MB of files)\n", out, dir, (unsigned long long)(romfs_dir_bytes(dir) >> 20));
        if (!romfs_build(dir, out, NULL, why, sizeof why)) { LOG("romfs: %s\n", why); return 1; }
        LOG("done\n");
        return 0;
    }
    // The game files: Super Mario Odyssey 1.0.0 (README). Not set up yet (first start), or no game code built for this
    // version of the app (an update): the setup page (runtime/ui/setup.mm) - the player's own files copied in, the game
    // code recompiled here. A source checkout has extracted/ and build/odyssey_game.* already.
    if (!setup_ready(root) && !setup_run(root)) return 0;
    if (!config_run()) return 0;  // (the configuration page: settings, then Play or Exit - runtime/ui/setup.mm)
    // (a source checkout: the RomFS image made from extracted/romfs first if it isn't there yet, as --build-romfs)
    {
        char dir[1100], out[1100], why[256];
        snprintf(dir, sizeof dir, "%s/romfs", root);
        snprintf(out, sizeof out, "%s/romfs.bin", root);
        FILE *f = fopen(out, "rb");
        if (f) fclose(f);
        else {
            LOG("building %s from %s (%llu MB of files, once)\n", out, dir, (unsigned long long)(romfs_dir_bytes(dir) >> 20));
            if (!romfs_build(dir, out, NULL, why, sizeof why)) FATAL("romfs: %s\n", why);
        }
    }
#ifndef __APPLE__
    {   // shaders listed by the last runs compiled ahead, before the game's device (runtime/nvn/vulkan/device.c)
        extern void vk_prepare_before_start(void);
        vk_prepare_before_start();
    }
#endif
    // SDL's event pump checks audio device events. Initialize the subsystem on
    // this thread before guest audio services can open streams.
    const char *no_audio = getenv("ODYSSEY_NO_AUDIO");
#ifdef __linux__
    // Linux: SDL's PulseAudio output first (PipeWire's own PulseAudio server on SteamOS and most distributions), then
    // PipeWire, then ALSA. SDL's PipeWire output dropped out at the game's start on several systems, which
    // SDL_AUDIO_DRIVER=pulseaudio fixed (#15). SDL_AUDIO_DRIVER still picks one.
    if (!getenv("SDL_AUDIO_DRIVER")) SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "pulseaudio,pipewire,alsa");
#endif
    if ((!no_audio || !strcmp(no_audio, "0")) && !SDL_InitSubSystem(SDL_INIT_AUDIO))
        LOG("[audio] SDL audio init failed: %s\n", SDL_GetError());
    if (getenv("ODYSSEY_TEST_CRASH") && strcmp(getenv("ODYSSEY_TEST_CRASH"), "missing-file"))
        SDL_DetachThread(SDL_CreateThread(test_crash, "testcrash", (void *)getenv("ODYSSEY_TEST_CRASH")));
    for (;;) {
        boot(root);
        ui_run();  // window and input on the main thread; returns when a reboot is requested
        teardown();
    }
}
