// Frame rate 120 or 90 (menu, Performance; ODYSSEY_NATIVE_120=1 / 90 / 0 sets it at start): a 120 or 90 Hz native draw
// loop with a 60 Hz simulation.
// 90: three draws for every two gameplay ticks, each showing the scene at its own point in time: Mario's pose and
// position go a fraction of a tick past the last tick (runtime/hle/odyssey_pose.c), the camera half a tick behind that,
// between the last two ticks' cameras (as at 120, where the draw-only frame shows Mario half a tick on and the camera
// at the last tick, the gameplay frame Mario at the tick and the camera half way back).
// The hooks are always in; the menu switches between it and the stock 60 FPS loop while the game runs.
// Keep the full framework update and draw path, including graphics housekeeping and the
// HUD, and run GameSystem::movement at most once per two VI ticks.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../rt.h"
#include "../odyssey_pose.h"
#include "../settings.h"
#include "../gameplay_clock.h"
#include "../ui/menu.h"
#include "../nvn/nvn.h"
#include "../nvn/rqueue.h"

static bool hooks_ok, trace, extra_draw;
uint64_t renderer_present_count(void);
// (proc_draw) a scene prepared for extra draws drew in this frame / did in the last gameplay frame (odyssey_native_hz)
static bool prepared_scene_drew, last_draw_known;
static bool safe_scene_load = true;
#define enabled odyssey_native_120_enabled()
static GuestFn original_game_movement, original_draw, original_movement, original_stage_draw, original_flight_draw;
static GuestFn original_demo_draw;
static GuestFn original_boot_intro, original_framework_calc, original_controller_calc;
static uint64_t texture_allocator, texture_calc, prepare_graphics;
static uint64_t last_logic_slot, logic_ticks, controller_ticks, draws, trace_ns;
static uint64_t simulation_tick, camera_matrix;
static float draw_lead;  // (90) this draw shows Mario this many ticks past the last tick: -1/6, 1/6 or 1/2
static uint64_t stage_draw_ns;
static GameplayClock recovery_clock;
static bool recover_60;
uint64_t vi_vsync_count(void);
uint64_t vi_vsync_time(uint64_t count);
uint64_t vi_wait_vsync(uint64_t target);
void input_native_tick_begin(void);
void input_native_tick_end(void);

bool odyssey_native_120_enabled(void) {
    if (!hooks_ok || settings_game_hz() == 60) return false;
    extern bool odyssey_scene_cadence_warming(void);
    // 1.4.1's asynchronous scene setup expects one framework draw per gameplay
    // tick. Extra loading draws can leave the first stage draw without a valid
    // temporary texture block (TextureDataEx::reset). Keep setup at the stock
    // cadence, then restore the requested rate after the scene's warm-up. This
    // also covers stage changes which reuse the same StageScene address.
    if (safe_scene_load && loader_main_is_v141() && odyssey_scene_cadence_warming()) return false;
    return true;
}
// Extra draws only in the scenes whose draw is prepared for them (the hooks below: a stage, the kingdom flight, the
// title screen, the opening). Any other scene - the painting warp's WorldWarpHoleScene faulted in its depth shadow
// map's texture allocation above 60 FPS (#72 follow-up: 1.0.0 at 165; #85: 1.4.1 at 240) - runs at the game's own 60
// while it shows: the timer and the cadence follow this rate, so it's the 60 FPS setting's path for those frames.
// ODYSSEY_EXTRA_DRAW_SCENES=all: every scene, as before.
static bool unprepared_scene(void) {
    static int known_only = -1;
    if (known_only < 0) { const char *e = getenv("ODYSSEY_EXTRA_DRAW_SCENES"); known_only = !(e && !strcmp(e, "all")); }
    return known_only && hooks_ok && !__atomic_load_n(&last_draw_known, __ATOMIC_RELAXED);
}
int odyssey_native_hz(void) {
    if (!odyssey_native_120_enabled()) return 60;
    if (unprepared_scene()) return 60;
#ifndef __APPLE__
    extern bool vk_native_brake(void);  // (vulkan/device.c: frames missing their time above 60 - a few seconds at 60)
    if (vk_native_brake()) return 60;
#endif
    return settings_fps_hz(g_settings.frame_rate);
}

bool odyssey_skip_extra_particle_update(void) { return enabled && extra_draw; }

bool odyssey_motion_enabled(void) {
    static int motion = -1;
    if (motion < 0) {
        const char *mode = getenv("ODYSSEY_MOTION_120");
        motion = !mode || !strcmp(mode, "1");
    }
    return motion != 0;
}

// 90: the camera between the last two ticks' (draw_lead + 1/2 ticks back from the last; the draw-only frames: none)
static void camera_draw_90(Cpu *c, uint64_t draw_scene, uint64_t source, float *pose) {
    static __thread uint64_t scene, camera, tick;  // the last tick's camera (last) and the one before (before)
    static __thread float last[9], before[9];
    static __thread bool paired;  // before: the tick before's, the same scene and camera
    if (!extra_draw) {
        paired = draw_scene == scene && source == camera && simulation_tick == tick + 1;
        memcpy(before, last, sizeof before);
        memcpy(last, pose, sizeof last);
        scene = draw_scene;
        camera = source;
        tick = simulation_tick;
    } else if (draw_scene != scene || source != camera || simulation_tick != tick) return;
    float back = 0.5f - draw_lead;
    if (!paired || back <= 0.001f) return;
    if (back > 1.0f) back = 1.0f;
    float travel = 0.0f, turn = 0.0f, alignment = 0.0f, length = 0.0f, next_length = 0.0f;
    for (unsigned i = 0; i < 3; i++) {
        float eye_delta = pose[i] - before[i], target_delta = pose[i + 3] - before[i + 3];
        float roll_delta = pose[i + 6] - before[i + 6];
        travel += eye_delta * eye_delta + target_delta * target_delta;
        turn += roll_delta * roll_delta;
        float direction = pose[i + 3] - pose[i];
        float next_direction = before[i + 3] - before[i];
        alignment += direction * next_direction;
        length += direction * direction;
        next_length += next_direction * next_direction;
    }
    // (the limits at 120, for a whole tick instead of half of one)
    if (!(travel <= 160.0f * 160.0f && turn <= 0.16f * 0.16f && (travel > 0.0f || turn > 0.0f))) return;
    if (!(alignment > 0.0f && alignment * alignment >= 0.95f * length * next_length)) return;
    for (unsigned i = 0; i < 9; i++) pose[i] += back * (before[i] - pose[i]);
    Cpu draw = *c;
    draw.x[0] = c->x[23];
    draw.x[1] = c->x[23] + 8;
    guest_call(&draw, camera_matrix);
}

void odyssey_camera_draw(Cpu *c) {
    // StageScene::drawMain's private copy of the current LookAtCamera.
    // Rebuild the view with sead's evaluator before the world and DLAA consume it.
    static __thread uint64_t scene, camera, tick;
    static __thread float previous[9];
    static __thread bool half_drawn;
    if (!enabled || !odyssey_motion_enabled() || !camera_matrix) return;
    uint64_t draw_scene = c->x[19], source = c->x[25];
    float *pose = (float *)(uintptr_t)(c->x[23] + 0x38);
    if (odyssey_native_hz() != 120) { camera_draw_90(c, draw_scene, source, pose); return; }  // (any rate but 120)
    if (extra_draw) {
        half_drawn = draw_scene == scene && source == camera && tick == simulation_tick;
        return;
    }
    bool continuous = draw_scene == scene && source == camera && half_drawn && simulation_tick == tick + 1;
    half_drawn = false;
    scene = draw_scene;
    camera = source;
    tick = simulation_tick;
    float old[9];
    memcpy(old, previous, sizeof old);
    memcpy(previous, pose, sizeof previous);
    if (!continuous) return;
    float travel = 0.0f, turn = 0.0f, alignment = 0.0f, length = 0.0f, next_length = 0.0f;
    for (unsigned i = 0; i < 3; i++) {
        float eye_delta = pose[i] - old[i], target_delta = pose[i + 3] - old[i + 3];
        float roll_delta = pose[i + 6] - old[i + 6];
        travel += eye_delta * eye_delta + target_delta * target_delta;
        turn += roll_delta * roll_delta;
        float direction = pose[i + 3] - pose[i];
        float next_direction = old[i + 3] - old[i];
        alignment += direction * next_direction;
        length += direction * direction;
        next_length += next_direction * next_direction;
    }
    // Skip cuts, abrupt changes in roll, and half steps that would reverse the view.
    if (!(travel <= 80.0f * 80.0f && turn <= 0.08f * 0.08f && (travel > 0.0f || turn > 0.0f))) return;
    if (!(alignment > 0.0f && alignment * alignment >= 0.95f * length * next_length)) return;
    for (unsigned i = 0; i < 9; i++) pose[i] += 0.5f * (old[i] - pose[i]);
    Cpu draw = *c;
    draw.x[0] = c->x[23];
    draw.x[1] = c->x[23] + 8;
    guest_call(&draw, camera_matrix);
}

static void quality_reduce_off(Cpu *c) { (void)c; }  // (dynamic resolution off: the Reduce nerve does nothing)
// ODYSSEY_LOG_QUALITY=1 (dynamic resolution on): each second with Reduce steps, how many - when the game lowers it
static GuestFn original_quality_reduce;

// Mouse camera, direct (#52): CameraPoserFollowLimit::update (the game's usual follow camera; vtable slot 5) adds its
// turn speed for this frame to its horizontal angle. The stick's speed comes eased (an accumulator that hands out half
// of itself each frame: the camera keeps turning after the stick is let go); the mouse's movement is added to that
// frame's speed as it is - the camera turns exactly as far as the mouse moved, at once. Vertically the stick moves a
// target the angle eases to (the holder at +0x150 / 1.4.1 +0x158: angle +0xc0, target +0xc4); the mouse moves both,
// within the stick's own limits (-35..85 degrees, measured: the game clamps only what the stick does).
// (input.c input_mouse_direct; ODYSSEY_MOUSE_DIRECT=0: through the stick as before; ODYSSEY_LOG_FOLLOW=1: the angles)
static GuestFn original_follow_update;
static uint32_t follow_speed_off, follow_angle_off, follow_vholder_off;  // (1.0.0: 0x1e4 / 0x1b8 / 0x150; 1.4.1: +8)
extern bool input_mouse_direct(float *dh, float *dv);
static void follow_update(Cpu *c) {
    uint64_t self = c->x[0];
    float dh = 0, dv = 0;
    if (input_mouse_direct(&dh, &dv)) {
        *(float *)(uintptr_t)(self + follow_speed_off) += dh;
        uint64_t holder = *(uint64_t *)(uintptr_t)(self + follow_vholder_off);
        if (dv != 0 && holder > 0x7000000000ull) {
            float *angle = (float *)(uintptr_t)(holder + 0xc0), *target = (float *)(uintptr_t)(holder + 0xc4);
            const float lo = -35, hi = 85;
            float t = *target + dv;
            if (dv < 0 && t < lo) t = *target < lo ? *target : lo;  // (never further out than it already is)
            if (dv > 0 && t > hi) t = *target > hi ? *target : hi;
            *angle += t - *target;
            *target = t;
        }
    }
    static int lg = -1;
    if (lg < 0) lg = getenv("ODYSSEY_LOG_FOLLOW") != NULL;
    if (lg) {
        static unsigned n;
        uint64_t holder = *(uint64_t *)(uintptr_t)(self + follow_vholder_off);
        if (n++ % 10 == 0 && holder > 0x7000000000ull)
            LOG("[follow] angle H %.2f (speed %.3f), V %.2f (target %.2f), mouse %.3f %.3f\n",
                *(float *)(uintptr_t)(self + follow_angle_off), *(float *)(uintptr_t)(self + follow_speed_off),
                *(float *)(uintptr_t)(holder + 0xc0), *(float *)(uintptr_t)(holder + 0xc4), dh, dv);
    }
    original_follow_update(c);
}
// The HUD at the output resolution (nvn/vulkan/exec.c "HUD layer"): nn::ui2d::Layout::Draw(DrawInfo&, gfx::CommandBuffer&)
// draws one layout - the HUD, menus, wipes - into the command buffer (the gfx one holds its NVNcommandBuffer* at +0xb8,
// both builds); its draws are marked there, so the renderer can draw them at the window's resolution after the scene
// was upscaled (DLSS / FSR / XeSS, a lower internal resolution) instead of upscaling them with it.
// ODYSSEY_HUD_MARKS=0: not marked.
static GuestFn original_layout_draw;
static _Atomic uint64_t boot_layout_ns;  // (boot_intro, Skip the start-up controller screen) its last call
static uint64_t boot_layout;             // the controller screen's layout (BootLoading, drawn while it runs)
static void layout_draw(Cpu *c) {
    {   // Skip the start-up controller screen: not drawn at all (black until the title) - the timer skip alone left it
        // up for as long as the first load takes (owner 2026-11-16: a second or more)
        uint64_t b = atomic_load(&boot_layout_ns), lay = c->x[0];
        if (b && plat_now_ns() - b < 3000000000ull && lay) {
            const char *name = *(const char **)(uintptr_t)(lay + 0x30);
            if (!boot_layout && name && (uintptr_t)name > 0x7000000000ull && !strcmp(name, "BootLoading")) boot_layout = lay;
            if (lay == boot_layout) return;
        }
    }
    static _Thread_local int depth;
    uint64_t ncb = c->x[2] ? *(uint64_t *)(uintptr_t)(c->x[2] + 0xb8) : 0;
    static int lg = -1;
    if (lg < 0) lg = getenv("ODYSSEY_LOG_LAYOUTS") != NULL;
    if (lg) {  // (probe) the layout, its root pane's fields and the strings its pointers lead to
        static uint64_t seen[256]; static int ns; static unsigned calls;
        uint64_t lay = c->x[0];
        bool known = false;
        for (int i = 0; i < ns; i++) known |= seen[i] == lay;
        if (!known && ns < 256) {
            seen[ns++] = lay;
            uint64_t root = *(uint64_t *)(uintptr_t)(lay + 0x18);
            char line[1024]; int n = snprintf(line, sizeof line, "[layout] %llx root %llx di %llx (y scale %g):", (unsigned long long)lay,
                                              (unsigned long long)root, (unsigned long long)c->x[1],
                                              c->x[1] ? *(float *)(uintptr_t)(c->x[1] + 0x14 + 16) : 0.0f);
            for (int o = 0x20; o < 0x70 && root; o += 4) n += snprintf(line + n, sizeof line - n, " %g", *(float *)(uintptr_t)(root + o));
            for (int o = 0; o < 0x80 && n < 900; o += 8) {
                uint64_t q = *(uint64_t *)(uintptr_t)(lay + o);
                if (q > 0x7000000000ull && q < 0x7fffffffffull) {
                    const char *str = (const char *)(uintptr_t)q; int k = 0;
                    while (k < 40 && str[k] >= 32 && str[k] < 127) k++;
                    if (k >= 4 && !str[k]) n += snprintf(line + n, sizeof line - n, " +%x'%s'", o, str);
                }
            }
            for (int o = 0x80; o < 0xa0 && root; o++) if (*(char *)(uintptr_t)(root + o) >= 'A') { n += snprintf(line + n, sizeof line - n, " pane@%x'%.24s'", o, (char *)(uintptr_t)(root + o)); break; }
            LOG("%s\n", line);
        }
        (void)calls;
    }
    // Anchored to the screen (the HUD: its counters, gauges, guides; a menu or message too - it covers the middle, the
    // renderer leaves it): the root pane (Layout +0x18) untranslated (+0x30) and the screen's size (+0x50) - not those
    // that follow something in the world (BalloonIcon: the root pane moved to it). The mini map's player marker
    // (MapMiniPlayer, a layout of its own placed on the map) goes with the map. Name: Layout +0x30.
    uint64_t lay = c->x[0], anchored = 0;
    uint64_t root = lay ? *(uint64_t *)(uintptr_t)(lay + 0x18) : 0;
    if (root) {
        const float *rp = (const float *)(uintptr_t)root;
        const char *name = *(const char **)(uintptr_t)(lay + 0x30);
        // (only the HUD's own layouts, by name: a menu's (the title's "Menu") sat on one side too, and moved away
        // from a logo another layout draws under it - the logo twice; owner 2026-11-16)
        bool named = name && (uintptr_t)name > 0x7000000000ull;
        bool map = named && !strncmp(name, "MapMini", 7);
        bool hud = named && (!strncmp(name, "Counter", 7) || !strncmp(name, "PlayGuide", 9) || !strncmp(name, "SaveMessage", 11));
        if (map || (hud && rp[12] == 0 && rp[13] == 0)) anchored = map ? 1 : lay;  // (the map's pieces: one id)
        (void)rp;
    }
    if (!depth++) nvn_ui_mark(c, ncb, 1, anchored);
    original_layout_draw(c);
    if (!--depth) nvn_ui_mark(c, ncb, 0, 0);
}
static void quality_reduce_log(Cpu *c) {
    static uint64_t window, n;
    uint64_t now = plat_now_ns();
    if (now - window > 1000000000ull) {
        if (n) LOG("[quality] %llu Reduce steps in the last second\n", (unsigned long long)n);
        window = now, n = 0;
    }
    n++;
    original_quality_reduce(c);
}

static void boot_intro(Cpu *c) {
    // HakoniwaStateBootLoadData::exeBootLayout starts controller setup on step zero.
    // Its transition requires both step > 540 and the asynchronous loader to be ready.
    int32_t *step = (int32_t *)(uintptr_t)(c->x[1] + 0x18);  // al::NerveKeeper::mStep
    bool first = *step == 0;
    atomic_store(&boot_layout_ns, plat_now_ns());  // (layout_draw: its screen not drawn)
    original_boot_intro(c);
    if (first && *step == 0) {
        *step = 540;  // NerveKeeper::update increments after this callback returns.
        LOG("[boot] skipped the controller instructions timer\n");
    }
}

// The gameplay tick (60 Hz) a vsync (90 or 120 Hz) falls in: a loop in the same one as the last draws only. A loop that
// comes two or three ticks on (a late frame) runs the next tick only, and the loops after it catch up the rest, so a
// device that can't hold the draw rate keeps gameplay at its 60 (the Steam Deck at 90 in Cap Kingdom, ~80 draws a
// second: 111-115 ticks in 2 s without this - the game itself 5% slow). Further behind (a load, a stall): dropped.
static uint64_t gameplay_slot(uint64_t vsync, uint64_t hz) {
    uint64_t slot = vsync * 60 / hz;
    if (last_logic_slot != UINT64_MAX && slot > last_logic_slot + 1 && slot <= last_logic_slot + 3) slot = last_logic_slot + 1;
    return slot;
}

static void framework_calc(Cpu *c) {
    // The method tree polls controllers before GameSystem::movement. Decide the tick
    // before either runs, so a draw-only iteration cannot consume a button edge.
    // ODYSSEY_NATIVE_120_STRICT=1 (tests): gameplay ticks and extra draws strictly alternate, whatever the time -
    // a frame series saved to disk (seconds per frame) shows each extra draw next to its gameplay frame
    static int strict = -1;
    if (strict < 0) strict = getenv("ODYSSEY_NATIVE_120_STRICT") != NULL;
    // A loop's own work (its time minus what it spent blocked: the present's vsync wait) - what an extra draw costs
    extern __thread uint64_t g_blocked_ns;
    static uint64_t prev_ns, prev_blocked, work_ns;
    uint64_t now = plat_now_ns(), blocked = g_blocked_ns;
    if (prev_ns && now - prev_ns < 100000000ull) {
        uint64_t w = now - prev_ns - (blocked - prev_blocked < now - prev_ns ? blocked - prev_blocked : 0);
        work_ns = work_ns ? (w > work_ns ? w : (work_ns * 7 + w) / 8) : w;  // (rises at once, falls slowly)
    }
    uint64_t hz = (uint64_t)odyssey_native_hz(), vsync = vi_vsync_count();
    static uint64_t previous_hz;
    if (hz != previous_hz) {
        // Counts are still monotonic when VI changes period; their mapping to
        // gameplay slots is not. A rate change starts a fresh cadence pair.
        last_logic_slot = UINT64_MAX;
        extra_draw = false;
        if (trace) LOG("[native-120] effective cadence %llu Hz\n", (unsigned long long)hz);
        previous_hz = hz;
    }
    uint64_t slot = gameplay_slot(vsync, hz);
    extra_draw = enabled && (strict ? !extra_draw : slot == last_logic_slot);
    // An extra draw only when it ends before the next gameplay tick is due: one that runs past it delays that tick's
    // frame, and in busy scenes (4K at the Cascade waterfall: ~13 ms a loop) gameplay frames then came 13 / 26 ms
    // apart - uneven motion. Then the loop waits for the tick instead (steady 60 there, 120 wherever it fits).
    // ODYSSEY_NATIVE_120_ADAPT=1 turns this on.
    static int adapt = -1;
    if (adapt < 0) adapt = getenv("ODYSSEY_NATIVE_120_ADAPT") && atoi(getenv("ODYSSEY_NATIVE_120_ADAPT")) != 0;  // (off unless asked:
    // measured at 4K without it, the Cascade Kingdom held 119-120 FPS; with it the busy parts ran at 95-110)
    if (extra_draw && !strict && adapt && last_logic_slot != UINT64_MAX) {
        uint64_t tick = ((last_logic_slot + 1) * hz + 59) / 60;  // (the first vsync of the next tick)
        uint64_t due = vi_vsync_time(tick);
        if (now + work_ns + work_ns / 8 + 500000 > due) {
            static uint64_t waits;
            if (trace && !(waits++ % 120)) LOG("[native-120] gameplay tick due in %.1f ms, a loop takes %.1f: waiting (%llu)\n",
                                                (double)(due > now ? due - now : 0) / 1e6, work_ns / 1e6, (unsigned long long)waits);
            uint64_t t0 = plat_now_ns();
            kernel_block_begin();  // (the core's other threads run meanwhile)
            vi_wait_vsync(tick);
            kernel_gate_reenter();  // (guest code runs next, from this hook)
            g_blocked_ns += plat_now_ns() - t0;
            vsync = vi_vsync_count();
            slot = gameplay_slot(vsync, hz);
            extra_draw = slot == last_logic_slot;
            blocked = g_blocked_ns;
            now = plat_now_ns();
        }
    }
    prev_ns = now, prev_blocked = blocked;
    last_logic_slot = slot;
    // 90: how far this loop's vsync is past its tick (0, 1/3 or 2/3; more when catching up), less 1/6: Mario 1/6 of a
    // tick behind that, the camera 2/3 - so that Mario never goes more than half a tick past the last tick, nor the
    // camera past it
    // Any other rate (72 .. 240) the same way: this vsync's fraction of a tick less (the largest fraction - 1/2), so
    // the leads run evenly from below the last tick up to half a tick past it, a vsync's time apart (240: -1/4, 0,
    // 1/4, 1/2; 144: -5/12 .. 1/2 in twelfths; 72: -1/3 .. 1/2), the camera always half a tick behind Mario
    if (hz != 120 && !strict) {
        uint64_t g = hz, b = 60;
        while (b) { uint64_t t = g % b; g = b; b = t; }  // (gcd: the fractions are steps of g / hz)
        float past = (float)(vsync * 60 / hz - slot) + (float)(vsync * 60 % hz) / (float)hz;
        float c0 = (float)(hz - g) / (float)hz - 0.5f;
        draw_lead = past - c0 > 0.5f ? 0.5f : past - c0;
    } else draw_lead = extra_draw ? 0.5f : 0.0f;
    // Private timing experiment, separate from the 90/120 rendering mode.
    // Repeat complete fixed updates, never a larger physics timestep. Keep
    // loading, non-stage scenes and the host settings menu on stock timing.
    extern bool odyssey_scene_cadence_warming(void);
    bool active = recover_60 && settings_game_hz() == 60 && stage_draw_ns && now-stage_draw_ns<100000000ull &&
                  !menu_is_open() && !odyssey_scene_cadence_warming();
    unsigned steps=gameplay_clock_steps(&recovery_clock,now,active);
    uint64_t framework=c->x[0];
    for (unsigned i=0;i<steps;i++) { c->x[0]=framework; original_framework_calc(c); }
}

static void controller_calc(Cpu *c) {
    if (enabled && extra_draw) return;
    if (!enabled) { if (recover_60) controller_ticks++; original_controller_calc(c); return; }
    controller_ticks++;
    input_native_tick_begin();
    original_controller_calc(c);
    input_native_tick_end();
}

static void game_movement(Cpu *c) {
    if (enabled && extra_draw) return;
    logic_ticks++;
    simulation_tick++;
    bool sample = enabled && odyssey_pose_enabled();
    if (sample) odyssey_pose_begin();
    original_game_movement(c);
    if (sample) odyssey_pose_end(simulation_tick);
}

// A second draw needs its own graphics preparation even when gameplay has not advanced: without it the dynamic
// texture allocator still holds the last draw's allocations and the depth shadow map's alloc faults (a stage, and
// the Odyssey's flight between kingdoms: DemoChangeWorldScene, DepthShadowMapDirector::drawToMask).
// (The allocator is reset for every extra draw, whatever scene draws: proc_draw. The title screen's TitleMenuScene,
// which draws the kingdom behind its menu, had no reset and faulted there - #72, 1.0.0 at 180 FPS. The scene hooks
// add the scene's own preparation.)
static void allocator_extra_draw(Cpu *c) {
    uint64_t allocator = *(const uint64_t *)(uintptr_t)texture_allocator;
    if (!allocator || !texture_calc) return;
    Cpu graphics = *c;
    graphics.x[0] = allocator;
    guest_call(&graphics, texture_calc);
}
static void prepare_extra_draw(Cpu *c, const char *what, bool *logged) {
    Cpu graphics = *c;
    graphics.x[0] = c->x[0];
    guest_call(&graphics, prepare_graphics);
    if (trace && !*logged) {
        LOG("[native-120] prepared graphics for an extra %s draw\n", what);
        *logged = true;
    }
}

static void stage_draw(Cpu *c) {
    stage_draw_ns=plat_now_ns();
    prepared_scene_drew = true;
    static bool logged;
    static uint64_t last_stage;
    if (enabled && extra_draw) {
        if (c->x[0] == last_stage && odyssey_pose_enabled()) odyssey_pose_draw(c, draw_lead);
        prepare_extra_draw(c, "stage", &logged);
    } else {
        last_stage = enabled ? c->x[0] : 0;
        // (90: the gameplay frames are between ticks too)
        if (last_stage && odyssey_native_hz() != 120 && draw_lead != 0.0f && odyssey_pose_enabled()) odyssey_pose_draw(c, draw_lead);
    }
    // (the renderer is told which of the tick's draws this is: vulkan/exec.c renderer_extra_frame)
    if (rq_enabled()) rq_push(RQ_EXTRA_FRAME, NULL, enabled && extra_draw);
    original_stage_draw(c);
}

static void flight_draw(Cpu *c) {
    static bool logged;
    prepared_scene_drew = true;
    if (enabled && extra_draw) prepare_extra_draw(c, "kingdom flight", &logged);
    original_flight_draw(c);
}

static GuestFn original_title_draw;
static void title_draw(Cpu *c) {  // (the title screen's scene: the kingdom drawn behind the menu)
    static bool logged;
    prepared_scene_drew = true;
    if (enabled && extra_draw) prepare_extra_draw(c, "title screen", &logged);
    original_title_draw(c);
}

static void demo_draw(Cpu *c) {
    static bool logged;
    // The new-game opening has its own Scene, rather than StageScene. Its extra
    // draw needs the same temporary graphics resources as stages and flights.
    prepared_scene_drew = true;
    if (enabled && extra_draw) prepare_extra_draw(c, "opening cutscene", &logged);
    original_demo_draw(c);
}

static void proc_draw(Cpu *c) {
    if (enabled && extra_draw) allocator_extra_draw(c);
    if (!extra_draw) prepared_scene_drew = false;
    original_draw(c);
    if (!extra_draw && prepared_scene_drew != last_draw_known) {
        static int nlog;
        if (enabled && nlog++ < 40)
            LOG("[native-120] %s (frame %llu)\n", prepared_scene_drew ? "a prepared scene again: extra draws on" :
                "a scene without extra-draw preparation: its own 60 FPS while it shows", (unsigned long long)renderer_present_count());
        __atomic_store_n(&last_draw_known, prepared_scene_drew, __ATOMIC_RELAXED);
    }
    draws++;
    if (!trace) return;
    uint64_t now = plat_now_ns();
    if (!trace_ns) trace_ns = now;
    if (now - trace_ns < 2000000000ull) return;
    LOG("[native-120] %.3fs, %llu simulation ticks, %llu native draws, %llu controller updates\n", (now - trace_ns) / 1e9,
        (unsigned long long)logic_ticks, (unsigned long long)draws, (unsigned long long)controller_ticks);
    trace_ns = now;
    draws = logic_ticks = controller_ticks = 0;
}

// Mario's position for the performance overlay (runtime/ui/menu.mm): al::LiveActor's pose keeper (+0x50), its
// translation (+8), kept as the floats' bits
static _Atomic uint32_t player_pos[3];
static _Atomic uint64_t player_pos_ns;
bool odyssey_player_position(float p[3]) {
    uint64_t at = atomic_load(&player_pos_ns);
    if (!at || plat_now_ns() - at > 500000000ull) return false;
    for (int i = 0; i < 3; i++) { uint32_t v = atomic_load(&player_pos[i]); memcpy(&p[i], &v, 4); }
    return true;
}

static void player_movement(Cpu *c) {
    static uint64_t updates;
    uint64_t actor = c->x[0];
    odyssey_pose_player(actor);
    original_movement(c);
    if (actor >= GUEST_LO && actor < GUEST_HI - 0x58) {
        uint64_t keeper = *(const uint64_t *)(uintptr_t)(actor + 0x50);
        if (keeper >= GUEST_LO && keeper < GUEST_HI - 0x14) {
            for (int i = 0; i < 3; i++) atomic_store(&player_pos[i], *(const uint32_t *)(uintptr_t)(keeper + 8 + 4 * i));
            atomic_store(&player_pos_ns, plat_now_ns());
        }
    }
    // Private diagnostic placement, enabled only by an explicit test variable.
    // Keep it confined to the verified 1.0.0 pose layout.
    static bool placed;
    extern uint64_t renderer_present_count(void);
    const char *place = getenv("ODYSSEY_TEST_PLAYER_POS");
    if (!placed && place) {
        unsigned long long frame;
        float p[3];
        const Module *main = loader_module("main");
        static const uint8_t v100[8] = {0x3c, 0xa1, 0x2d, 0xfa, 0xaf, 0x9c, 0x82, 0xda};
        if (main && !memcmp(main->build_id, v100, sizeof v100) &&
            sscanf(place, "%llu:%f:%f:%f", &frame, &p[0], &p[1], &p[2]) == 4 && renderer_present_count() >= frame) {
            uint64_t pose = *(const uint64_t *)(uintptr_t)(actor + 0x50);
            if (pose >= GUEST_LO && pose < GUEST_HI - 20) {
                Cpu saved = *c;
                c->x[0] = actor;
                c->sp = (c->sp - 0x400) & ~0xFull;
                uint64_t get = loader_find_symbol("_ZN19PlayerActorHakoniwa15getPlayerPuppetEv");
                uint64_t reset = loader_find_symbol("_ZN12PlayerPuppet13resetPositionERKN4sead7Vector3IfEE");
                if (get && reset) {
                    guest_call(c, get);
                    if (c->x[0]) {
                        c->x[1] = c->sp + 0x200;
                        memcpy((void *)(uintptr_t)c->x[1], p, sizeof p);
                        guest_call(c, reset);
                        placed = true;
                    }
                }
                *c = saved;
                if (placed) LOG("[test-place] frame %llu position %.3f %.3f %.3f\n", frame, p[0], p[1], p[2]);
            }
        }
    }
    if (!trace || ++updates % 15) return;
    // 1.0.0 LiveActor::mPoseKeeper and al::getTrans; read the simulation position after movement.
    uint64_t pose = *(const uint64_t *)(uintptr_t)(actor + 0x50);
    if (!pose) return;
    const float *pos = (const float *)(uintptr_t)(pose + 8);
    LOG("[native-120] player %.3fs tick %llu position %.3f %.3f %.3f\n", plat_now_ns() / 1e9,
        (unsigned long long)updates, pos[0], pos[1], pos[2]);
}

static GuestFn original_szs_device;
static void szs_device(Cpu *c) {  // (sead::SZSDecompressor::tryDecompFromDevice: read + decode, off the core gate)
    static bool logged;
    if (!logged) { logged = true; LOG("[szs] file decompression runs off the core gate\n"); }
    kernel_ungated_begin();
    original_szs_device(c);
    kernel_ungated_end();
}

static GuestFn original_szs_parallel;
static void szs_parallel(Cpu *c) {  // (sead::ParallelSZSDecompressor::tryDecompFromDevice: off the core gate)
    static bool logged;
    if (!logged) { logged = true; LOG("[szs] parallel file decompression runs off the core gate\n"); }
    kernel_ungated_begin();
    original_szs_parallel(c);
    kernel_ungated_end();
}

static GuestFn original_szs_decoder;
static void szs_decoder(Cpu *c) {  // (1.4.1 0x97dae0: the decoder's virtual entry, off the core gate)
    static bool logged;
    if (!logged) { logged = true; LOG("[szs] the decoder's own entry runs off the core gate\n"); }
    kernel_ungated_begin();
    original_szs_decoder(c);
    kernel_ungated_end();
}

static GuestFn original_szs_calc;
static void szs_calc(Cpu *c) {
    static bool logged;
    if (!logged) { logged = true; LOG("[szs] decompression runs off the core gate\n"); }
    kernel_ungated_begin();  // (the core's other threads run meanwhile)
    original_szs_calc(c);
    kernel_ungated_end();  // (guest code runs next)
}

void odyssey_native_120_boot(void) {
    stage_draw_ns=0;
    recovery_clock=(GameplayClock){0};
    const char *recovery=getenv("ODYSSEY_GAMEPLAY_CATCHUP");
    // On by default everywhere (ODYSSEY_GAMEPLAY_CATCHUP=0: off): below 60 FPS the game runs extra ticks so it keeps
    // real-time speed instead of slowing down. Windows, forced ~32 FPS (2026-11-15): 120 ticks per 2 s with it, 60
    // without (half speed - the slow motion of #13 and #42); Steam Deck likewise.
    recover_60=!recovery || atoi(recovery)!=0;
    static bool installed;
    if (installed) {
        last_logic_slot = UINT64_MAX;
        extra_draw = false;
        return;
    }
    // ODYSSEY_NATIVE_120=1 / 90 / 0 sets the initial frame rate for tests.
    settings_load();
    const char *mode = getenv("ODYSSEY_NATIVE_120");
    if (mode && *mode) {  // (0: 60; 1: 120; else frames per second)
        int want = !strcmp(mode, "0") ? 60 : !strcmp(mode, "1") ? 120 : atoi(mode);
        for (int i = 0; i < FPS_COUNT; i++) if (settings_fps_hz(i) == want) g_settings.frame_rate = i;
    }
    bool requested = true;  // (the hooks: always, the setting switches them)
    const char *skip = getenv("ODYSSEY_SKIP_CONTROLLER_INTRO");
    bool skip_intro = skip ? !strcmp(skip, "1") : g_settings.skip_intro;  // (the menu's Skip controller tips)
    trace = getenv("ODYSSEY_NATIVE_120_TRACE") != NULL;
    const char *safe_load = getenv("ODYSSEY_NATIVE_120_SAFE_LOAD");
    safe_scene_load = !safe_load || atoi(safe_load) != 0;
    if (!requested && !trace && !skip_intro) return;

    const Module *main = loader_module("main");
    const uint8_t build_id[] = {0x3c, 0xa1, 0x2d, 0xfa, 0xaf, 0x9c, 0x82, 0xda};
    bool v141 = loader_main_is_v141();
    if (!main || (memcmp(main->build_id, build_id, sizeof build_id) && !v141)) {
        LOG("[native-120] requires Odyssey 1.0.0 or 1.4.1\n");
        return;
    }
    if (skip_intro) {
        // virtual Nerve callback for exeBootLayout: slot 0 of (anonymous)::HostTypeNrvBootLayout's vtable (1.4.1: the
        // same two instructions there, tools/dis141.py)
        uint64_t callback = main->base + (v141 ? 0x4b7a30 : 0x5117bc);
        original_boot_intro = dispatch_lookup(callback);
        if (original_boot_intro) hle_override(callback, boot_intro, "Odyssey controller intro timer");
        else LOG("[boot] controller instructions callback unavailable\n");
    }
    if (v141) {
        // sead::ParallelSZSDecompressor::DecompThread::calc (its vtable, tools/match141.py): decodes a chunk of a
        // compressed file in a leaf function of its own buffers, then signals the reader. The thread (priority 46)
        // shares a core with game threads of a higher priority, which on the Switch preempt it at once; here a core
        // changes hands only at a function call, and the decode loop has none - a game thread waited ~20 ms for the
        // core whenever a model or effect loaded (hitting things, busy scenes): a late frame. The decode runs without
        // the core now (the signal after it takes the core back). ODYSSEY_SZS_UNGATED=0: as before.
        const char *e = getenv("ODYSSEY_SZS_UNGATED");
        uint64_t decomp = main->base + 0x97e420;
        if ((!e || atoi(e) != 0) && (original_szs_calc = dispatch_lookup(decomp)))
            hle_override(decomp, szs_calc, "SZS decompression off the core gate");
    }
    {   // sead::SZSDecompressor::tryDecompFromDevice (its vtable; 1.0.0 by name): a loading thread (priority 45) reads a
        // compressed file and decodes it in a loop without calls, holding its core - the game's main thread (44, on the
        // same core) waited 12-26 ms for it whenever something new loaded (a model, an effect): the stutters when new
        // things appear (Steam Deck, [gate] log). On the Switch the higher-priority thread would preempt it at once. It
        // runs off the core gate now, as the parallel decompressor's threads (above). ODYSSEY_SZS_UNGATED=0: as before.
        // The same for sead::ParallelSZSDecompressor::tryDecompFromDevice: the loading thread decodes parts of the file
        // itself next to the decompressor's threads - that was the one holding the core in the [gate] log (thread 83).
        const char *e = getenv("ODYSSEY_SZS_UNGATED");
        uint64_t dev = main->base + (v141 ? 0x97d5c0 : 0xb63d84), par = main->base + (v141 ? 0x97df50 : 0xb622bc);
        if ((!e || atoi(e) != 0) && (original_szs_device = dispatch_lookup(dev)))
            hle_override(dev, szs_device, "SZS file decompression off the core gate");
        if ((!e || atoi(e) != 0) && (original_szs_parallel = dispatch_lookup(par)))
            hle_override(par, szs_parallel, "Parallel SZS file decompression off the core gate");
        // ... and the decoder's own virtual entry (1.4.1: sead::Decompressor vtable slot 1, 4 bytes before the decode
        // loop it runs into): the [gate] log still showed thread 83 holding the core inside the decoder
        uint64_t dec = main->base + 0x97dae0;
        if (v141 && (!e || atoi(e) != 0) && (original_szs_decoder = dispatch_lookup(dec)))
            hle_override(dec, szs_decoder, "SZS decoder off the core gate");
    }
    {   // the HUD's draws marked (layout_draw)
        uint64_t ld = main->base + (v141 ? 0x903d60 : 0xb02b2c);
        const char *e = getenv("ODYSSEY_HUD_MARKS");
        if ((!e || atoi(e) != 0) && (original_layout_draw = dispatch_lookup(ld)))
            hle_override(ld, layout_draw, "Odyssey layouts: their draws marked (the HUD at the output resolution)");
    }
    {   // the mouse camera, direct (follow_update)
        uint64_t upd = main->base + (v141 ? 0xbd2b0 : 0xca8b0);
        follow_speed_off = v141 ? 0x1ec : 0x1e4, follow_angle_off = v141 ? 0x1c0 : 0x1b8, follow_vholder_off = v141 ? 0x158 : 0x150;
        const char *e = getenv("ODYSSEY_MOUSE_DIRECT");
        if ((!e || atoi(e) != 0) && (original_follow_update = dispatch_lookup(upd)))
            hle_override(upd, follow_update, "Odyssey follow camera: mouse turns it directly");
    }
    {   // Dynamic resolution off (setting game_dynres; ODYSSEY_GAME_DYNRES=0/1): al::GraphicsQualityController - the game's
        // own load-driven quality, a nerve machine Wait -> Reduce -> Recover - lowers the resolution when frames run
        // long (on a Steam Deck at 7-10 W down to 960x540 of 1600x900, or a smaller viewport in handheld: blurry). Its
        // Reduce nerve's execute (vtable slot 0 of (anonymous)::GraphicsQualityControllerNrvReduce, found from the RTTI
        // name in both builds) does nothing then: the quality never drops, so Recover has nothing to undo.
        uint64_t reduce = main->base + (v141 ? 0x5ef6d0 : 0xa57f58);
        if (!settings_game_dynres() && dispatch_lookup(reduce)) {
            hle_override(reduce, quality_reduce_off, "Odyssey dynamic resolution off");
            LOG("[quality] the game's dynamic resolution is off (full resolution)\n");
        } else if (getenv("ODYSSEY_LOG_QUALITY") && (original_quality_reduce = dispatch_lookup(reduce)))
            hle_override(reduce, quality_reduce_log, "Odyssey dynamic resolution log");
    }
    if (!requested && !trace) {
        installed = true;
        return;
    }
    uint64_t game_move = loader_find_symbol("_ZN10GameSystem8movementEv");
    uint64_t draw = loader_find_symbol("_ZN2al15GameFrameworkNx9procDraw_Ev");
    uint64_t movement = loader_find_symbol("_ZN19PlayerActorHakoniwa8movementEv");
    uint64_t stage = loader_find_symbol("_ZNK10StageScene8drawMainEv");
    uint64_t flight = loader_find_symbol("_ZNK20DemoChangeWorldScene8drawMainEv");
    uint64_t demo = loader_find_symbol("_ZNK9DemoScene8drawMainEv");
    uint64_t title = loader_find_symbol("_ZNK14TitleMenuScene8drawMainEv");
    uint64_t calc = loader_find_symbol("_ZN4sead15GameFrameworkNx9procCalc_Ev");
    uint64_t controller = loader_find_symbol("_ZN4sead13ControllerMgr4calcEv");
    texture_allocator = loader_find_symbol("_ZN3agl3utl23DynamicTextureAllocator9sInstanceE");
    texture_calc = loader_find_symbol("_ZN3agl3utl23DynamicTextureAllocator4calcEv");
    prepare_graphics = loader_find_symbol("_ZN2al18updateGraphicsPrevEPNS_5SceneE");
    if (odyssey_motion_enabled())
        camera_matrix = loader_find_symbol("_ZNK4sead12LookAtCamera14doUpdateMatrixEPNS_8Matrix34IfEE");
    original_game_movement = dispatch_lookup(game_move);
    original_draw = dispatch_lookup(draw);
    original_movement = dispatch_lookup(movement);
    original_stage_draw = dispatch_lookup(stage);
    original_flight_draw = flight ? dispatch_lookup(flight) : NULL;
    original_demo_draw = demo ? dispatch_lookup(demo) : NULL;
    original_title_draw = title ? dispatch_lookup(title) : NULL;
    original_framework_calc = dispatch_lookup(calc);
    original_controller_calc = dispatch_lookup(controller);
    // (1.4.1 has no DynamicTextureAllocator::calc - the game never called it and the build dropped it: the extra draws
    // go without it there; prepare_extra_draw skips it)
    if (v141 && !texture_calc) LOG("[native-120] 1.4.1: no DynamicTextureAllocator::calc; extra draws without it\n");
    if (!original_game_movement || !original_draw || (trace && !original_movement) ||
        (requested && (!original_stage_draw || !original_framework_calc || !original_controller_calc ||
                       !texture_allocator || (!texture_calc && !v141) || !prepare_graphics))) {
        LOG("[native-120] required game hooks are unavailable\n");
        return;
    }
    last_logic_slot = UINT64_MAX;
    hooks_ok = true;
    if (original_demo_draw) hle_override(demo, demo_draw, "Odyssey opening cutscene draw");
    else LOG("[native-120] opening cutscene draw hook unavailable\n");
    hle_override(game_move, game_movement, "Odyssey simulation cadence");
    {
        hle_override(calc, framework_calc, "Odyssey update cadence");
        hle_override(controller, controller_calc, "Odyssey controller cadence");
        hle_override(stage, stage_draw, "Odyssey extra draw graphics preparation");
        if (original_flight_draw) hle_override(flight, flight_draw, "Odyssey extra flight draw graphics preparation");
        else LOG("[native-120] kingdom flight draw hook unavailable\n");
        if (original_title_draw) hle_override(title, title_draw, "Odyssey extra title screen draw graphics preparation");
        else LOG("[native-120] title screen draw hook unavailable\n");
    }
    hle_override(draw, proc_draw, "Odyssey extra draws: the texture allocator reset (and the native draw trace)");
    // (always: the performance overlay shows Mario's position)
    if (original_movement) hle_override(movement, player_movement, "Odyssey player movement observation");
    installed = true;
    if (settings_game_hz() != 60)
        LOG("[native-120] %d Hz selected, 60 Hz gameplay%s\n", settings_fps_hz(g_settings.frame_rate),
            v141 ? "; stock cadence during scene setup" : "");
    else LOG("[native-120] stock 60 FPS (switchable in the menu)\n");
}
