// A separately assigned second controller. SDL access stays on the event thread;
// guest HID reads consume atomic samples and cannot steal player one's pad.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "../input_players.h"
#include "../input_family.h"
#include "../input_bind.h"
#include "../settings.h"
#include "../rt.h"

static SDL_Gamepad *cappy;
static atomic_uint connected, requested;
static atomic_bool request_pending;
static atomic_bool assigned;
static atomic_int family;
static atomic_uint_fast64_t buttons, presses;
static atomic_int axes[4];
static atomic_uint generation;
static _Thread_local uint64_t tick_buttons;
static _Thread_local bool tick_active;
static _Thread_local unsigned tick_generation;
static char reconnect_path[1024], reconnect_serial[256];
static SDL_GUID reconnect_guid;
static bool reconnect;
static atomic_uint rumble_low, rumble_high;
static atomic_bool rumble_pending;
extern bool odyssey_native_120_enabled(void);
extern void input_cappy_style_changed(void);
extern void input_cappy_test_primary(uint32_t id);
extern uint64_t renderer_present_count(void);
static atomic_uint test_ids[2];
bool input_cappy_test_device(uint32_t id) { return id && (id == test_ids[0] || id == test_ids[1]); }

// Opt-in gameplay fixture: process-local virtual pads, never a physical device.
// P2 moves at frames 4000-4300; disconnect/reconnect at 5500/5700.
static void test_frame(void) {
    static bool initialized, removed, restored;
    static SDL_Joystick *joystick;
    static SDL_VirtualJoystickDesc desc;
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("ODYSSEY_CAPPY_TEST") != NULL;
    if (!enabled) return;
    if (!initialized) {
        initialized = true;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.vendor_id = 0x1209; desc.product_id = 1;
        desc.naxes = 6; desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.axis_mask = 0x3f; desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        desc.name = "Private test Mario";
        test_ids[0] = SDL_AttachVirtualJoystick(&desc);
        if (test_ids[0]) SDL_OpenGamepad(test_ids[0]);
        input_cappy_test_primary(test_ids[0]);
        desc.product_id = 2; desc.name = "Private test Cappy";
        test_ids[1] = SDL_AttachVirtualJoystick(&desc);
        joystick = SDL_OpenJoystick(test_ids[1]);
        input_cappy_assign(test_ids[1]);
        LOG("[cappy-test] virtual controllers %u/%u\n", (unsigned)test_ids[0], (unsigned)test_ids[1]);
    }
    uint64_t frame = renderer_present_count();
    if (!removed && frame >= 5500) {
        removed = true;
        if (joystick) SDL_CloseJoystick(joystick);
        joystick = NULL;
        SDL_DetachVirtualJoystick(test_ids[1]);
        LOG("[cappy-test] secondary disconnected, primary %u\n", input_primary_id());
    }
    if (!restored && frame >= 5700) {
        restored = true;
        test_ids[1] = SDL_AttachVirtualJoystick(&desc);
        joystick = SDL_OpenJoystick(test_ids[1]);
        LOG("[cappy-test] secondary reattached as %u, primary %u\n", (unsigned)test_ids[1], input_primary_id());
    }
    if (joystick) {
        bool camera_test = getenv("ODYSSEY_CAPPY_CAMERA_TEST") != NULL;
        if (camera_test) {
            SDL_Gamepad *mario = SDL_GetGamepadFromID(test_ids[0]);
            SDL_Joystick *main_stick = mario ? SDL_GetGamepadJoystick(mario) : NULL;
            if (main_stick) SDL_SetJoystickVirtualAxis(main_stick, SDL_GAMEPAD_AXIS_RIGHTX,
                frame >= 4500 && frame < 4700 ? 20000 : frame >= 3800 && frame < 5000 ? 500 : 0);
            SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTX,
                frame >= 4000 && frame < 4200 ? 24000 : frame >= 4500 && frame < 4700 ? -24000 : 0);
            SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTY,
                frame >= 4300 && frame < 4400 ? -24000 : 0);
        }
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX,
            !camera_test && ((frame >= 4000 && frame < 4300) || (frame >= 5800 && frame < 6000)) ? 24000 : 0);
        SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_EAST,
            (frame >= 4050 && frame < 4110) || (frame >= 5850 && frame < 5890));
    }
}

uint32_t input_cappy_id(void) { return atomic_load(&connected); }
bool input_cappy_assigned(void) { return atomic_load(&assigned); }
int input_cappy_family(void) {
    static int forced = -2;
    if (forced == -2) {
        const char *test = getenv("ODYSSEY_CAPPY_PAD_TYPE");
        forced = !test ? -1 : !strcmp(test, "playstation") ? 2 : !strcmp(test, "xbox") ? 1 : 0;
    }
    return forced < 0 ? atomic_load(&family) : forced;
}
void input_cappy_assign(uint32_t id) { atomic_store(&requested, id); atomic_store(&request_pending, true); }

static void clear_sample(void) {
    atomic_fetch_add(&generation, 1);
    atomic_store(&buttons, 0);
    atomic_store(&presses, 0);
    atomic_store(&rumble_pending, false);
    for (int i = 0; i < 4; i++) atomic_store(&axes[i], 0);
}

static void disconnect(void) {
    atomic_store(&connected, 0);
    clear_sample();
    if (cappy) { SDL_RumbleGamepad(cappy, 0, 0, 0); SDL_CloseGamepad(cappy); cappy = NULL; }
    input_cappy_style_changed();
}

static void connect_pad(SDL_JoystickID id) {
    SDL_Gamepad *p = SDL_OpenGamepad(id);
    if (!p) return;
    cappy = p;
    atomic_store(&family, input_controller_family(p));
    clear_sample();
    reconnect_guid = SDL_GetGamepadGUIDForID(id);
    const char *path = SDL_GetGamepadPath(p), *serial = SDL_GetGamepadSerial(p);
    snprintf(reconnect_path, sizeof reconnect_path, "%s", path ? path : "");
    snprintf(reconnect_serial, sizeof reconnect_serial, "%s", serial ? serial : "");
    reconnect = true;
    atomic_store(&assigned, true);
    atomic_store(&connected, id);
    SDL_SetGamepadPlayerIndex(p, 1);
    LOG("[cappy] controller assigned: %s (SDL %u)\n", SDL_GetGamepadName(p), id);
    input_cappy_style_changed();
}

static void read_pad(void) {
    if (!cappy) return;
    uint32_t host = 0;
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
        if (SDL_GetGamepadButton(cappy, (SDL_GamepadButton)i)) host |= 1u << i;
    if (SDL_GetGamepadAxis(cappy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) host |= 1u << PADB_LTRIGGER;
    if (SDL_GetGamepadAxis(cappy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) host |= 1u << PADB_RTRIGGER;
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (g_bind_pad[i] >= 0 && g_bind_pad[i] < PADB_HOST_COUNT && (host >> g_bind_pad[i] & 1)) b |= 1ull << i;
    uint64_t before = atomic_exchange(&buttons, b);
    if (odyssey_native_120_enabled()) atomic_fetch_or(&presses, b & ~before);
    for (int pair = 0; pair < 2; pair++) {
        float x = SDL_GetGamepadAxis(cappy, (SDL_GamepadAxis)(pair * 2)) / 32767.0f;
        float y = -SDL_GetGamepadAxis(cappy, (SDL_GamepadAxis)(pair * 2 + 1)) / 32767.0f;
        stick_shape(&x, &y, pair == 1);
        atomic_store(&axes[pair * 2], (int)lroundf(fmaxf(-1, fminf(x, 1)) * 32767));
        atomic_store(&axes[pair * 2 + 1], (int)lroundf(fmaxf(-1, fminf(y, 1)) * 32767));
    }
}

void input_cappy_service(uint32_t primary) {
    test_frame();
    if (atomic_exchange(&request_pending, false)) {
        uint32_t id = atomic_load(&requested);
        if (!id || id != primary) {
            atomic_store(&assigned, false);
            reconnect = false;
            disconnect();
            if (id) connect_pad(id);
        }
    }
    if (cappy && !SDL_GamepadConnected(cappy)) disconnect();
    if (!cappy && reconnect) {
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        SDL_JoystickID match = 0;
        for (int i = 0; i < n; i++) {
            if (ids[i] == primary) continue;
            SDL_GUID guid = SDL_GetGamepadGUIDForID(ids[i]);
            if (memcmp(&guid, &reconnect_guid, sizeof guid)) continue;
            SDL_Gamepad *p = SDL_GetGamepadFromID(ids[i]);
            const char *serial = p ? SDL_GetGamepadSerial(p) : NULL;
            const char *path = SDL_GetGamepadPathForID(ids[i]);
            if (reconnect_serial[0] ? (!serial || strcmp(serial, reconnect_serial)) :
                (reconnect_path[0] && (!path || strcmp(path, reconnect_path)))) continue;
            if (match) { match = 0; break; } // ambiguous: let the player choose again
            match = ids[i];
        }
        SDL_free(ids);
        if (match) connect_pad(match);
    }
    read_pad();
    if (cappy && atomic_exchange(&rumble_pending, false))
        SDL_RumbleGamepad(cappy, (Uint16)atomic_load(&rumble_low), (Uint16)atomic_load(&rumble_high), 250);
}

bool input_cappy_event(const SDL_Event *e) {
    uint32_t id = atomic_load(&connected);
    if (!id) return false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.which == id) {
        uint64_t b = 0;
        for (int i = 0; i < BIND_PAD_COUNT; i++) if (g_bind_pad[i] == e->gbutton.button) b |= 1ull << i;
        if (odyssey_native_120_enabled()) atomic_fetch_or(&presses, b);
    }
    bool ours = (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e->type == SDL_EVENT_GAMEPAD_BUTTON_UP) ? e->gbutton.which == id :
                e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION ? e->gaxis.which == id : false;
    if (ours) read_pad();
    return ours;
}

void input_cappy_tick_begin(void) {
    tick_generation = atomic_load(&generation);
    tick_buttons = atomic_load(&buttons) | atomic_exchange(&presses, 0);
    tick_active = true;
}
void input_cappy_tick_end(void) { tick_active = false; }
void input_cappy_reset_presses(void) {
    atomic_store(&presses, 0);
    atomic_fetch_add(&generation, 1);
}

bool input_cappy_state(void *state, bool menu) {
    if (!input_cappy_id()) return false;
    uint8_t *s = state;
    uint64_t b = menu ? 0 : tick_active && tick_generation == atomic_load(&generation) ? tick_buttons : atomic_load(&buttons);
    for (int i = 0; i < 4; i++) {
        int value = menu ? 0 : atomic_load(&axes[i]);
        memcpy(s + 16 + i * 4, &value, 4);
    }
    // nn::hid direction bits are L: left/up/right/down, R: left/up/right/down.
    for (int pair = 0; pair < 2; pair++) {
        int x = menu ? 0 : atomic_load(&axes[pair * 2]), y = menu ? 0 : atomic_load(&axes[pair * 2 + 1]);
        if (x < -16000) b |= 1ull << (16 + pair * 4);
        if (y > 16000) b |= 1ull << (17 + pair * 4);
        if (x > 16000) b |= 1ull << (18 + pair * 4);
        if (y < -16000) b |= 1ull << (19 + pair * 4);
    }
    memcpy(s + 8, &b, 8);
    uint32_t attr = 1;
    memcpy(s + 32, &attr, 4);
    if (getenv("ODYSSEY_CAPPY_TRACE")) {
        static uint64_t last;
        if (b != last) LOG("[cappy] HID buttons %#llx, left %d %d, menu %d\n", (unsigned long long)b,
            menu ? 0 : atomic_load(&axes[0]), menu ? 0 : atomic_load(&axes[1]), menu);
        last = b;
    }
    return true;
}

void input_cappy_rumble(float low, float high) {
    atomic_store(&rumble_low, (unsigned)lroundf(fminf(fmaxf(low, 0), 1) * 65535));
    atomic_store(&rumble_high, (unsigned)lroundf(fminf(fmaxf(high, 0), 1) * 65535));
    atomic_store(&rumble_pending, true);
}
