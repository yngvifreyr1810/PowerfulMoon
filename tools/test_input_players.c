// Process-local SDL virtual controllers exercise the real secondary input path.
// No game data, physical controller or system configuration is modified.
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "input_bind.h"
#include "input_players.h"
#include "settings.h"

Settings g_settings = {.stick_deadzone = 15};
int16_t g_bind_pad[BIND_PAD_COUNT] = {SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH,
    SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    PADB_LTRIGGER, PADB_RTRIGGER, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_DOWN};
static bool native;
static int notifications;
static Uint16 rumble_l, rumble_h;
bool odyssey_native_120_enabled(void) { return native; }
void input_cappy_style_changed(void) { notifications++; }
void input_cappy_test_primary(uint32_t id) { (void)id; }
uint32_t input_primary_id(void) { return 0; }
uint64_t renderer_present_count(void) { return 0; }

static bool SDLCALL rumble(void *p, Uint16 l, Uint16 h) { (void)p; rumble_l = l; rumble_h = h; return true; }
static SDL_JoystickID attach(unsigned product) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x1209; desc.product_id = product;
    desc.naxes = 6; desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = 0x3f; desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.name = product == 1 ? "Test Mario" : "Test Cappy";
    desc.Rumble = rumble;
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    assert(id);
    return id;
}
typedef struct { int64_t sample; uint64_t buttons; int32_t axes[4]; uint32_t attributes, padding; } State;
static State state(bool menu) { State s = {0}; assert(input_cappy_state(&s, menu)); return s; }
static void events(void) {
    SDL_UpdateJoysticks();
    SDL_Event e;
    while (SDL_PollEvent(&e)) input_cappy_event(&e);
}

int main(void) {
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_JoystickID mario = attach(1), cappy = attach(2);
    SDL_Gamepad *p1 = SDL_OpenGamepad(mario);
    SDL_Joystick *j1 = SDL_GetGamepadJoystick(p1), *j2 = SDL_OpenJoystick(cappy);
    assert(p1 && j1 && j2);
    input_cappy_assign(mario); input_cappy_service(mario);
    assert(!input_cappy_id()); // the same pad cannot control both players
    input_cappy_assign(cappy); input_cappy_service(mario);
    assert(input_cappy_id() == cappy && input_cappy_assigned() && notifications >= 1);
    assert(SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_EAST, true));
    assert(SDL_SetJoystickVirtualAxis(j2, SDL_GAMEPAD_AXIS_LEFTX, 24000));
    assert(SDL_SetJoystickVirtualAxis(j2, SDL_GAMEPAD_AXIS_LEFTY, -24000));
    assert(SDL_SetJoystickVirtualButton(j1, SDL_GAMEPAD_BUTTON_SOUTH, true));
    events(); input_cappy_service(mario);
    State s = state(false);
    assert(s.attributes == 1 && (s.buttons & 1) && !(s.buttons & 2));
    assert(s.axes[0] > 16000 && s.axes[1] > 16000);
    assert((s.buttons & (1ull << 17)) && (s.buttons & (1ull << 18)) && !(s.buttons & (1ull << 16)));
    assert(SDL_GetGamepadButton(p1, SDL_GAMEPAD_BUTTON_SOUTH)); // P1's independent sample is untouched
    s = state(true);
    assert(!s.buttons && !s.axes[0] && !s.axes[1] && s.attributes == 1);
    assert(SDL_SetJoystickVirtualAxis(j2, SDL_GAMEPAD_AXIS_LEFTX, 2000));
    assert(SDL_SetJoystickVirtualAxis(j2, SDL_GAMEPAD_AXIS_LEFTY, -2000));
    events(); input_cappy_service(mario);
    s = state(false); assert(!s.axes[0] && !s.axes[1]); // radial deadzone

    // A press and release between gameplay polls remains visible for one tick.
    assert(SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_EAST, false));
    events(); input_cappy_service(mario);
    native = true;
    assert(SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_EAST, true)); events();
    assert(SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_EAST, false)); events();
    input_cappy_tick_begin(); assert(state(false).buttons & 1); assert(state(false).buttons & 1);
    input_cappy_tick_end(); assert(!(state(false).buttons & 1));
    input_cappy_tick_begin(); assert(!(state(false).buttons & 1)); input_cappy_tick_end();
    assert(SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_EAST, true)); events();
    input_cappy_tick_begin();
    assert(SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_EAST, false)); events();
    assert(state(false).buttons & 1);
    input_cappy_reset_presses(); assert(!(state(false).buttons & 1)); // menu takes ownership mid-tick
    input_cappy_tick_end();

    g_bind_pad[BIND_A] = SDL_GAMEPAD_BUTTON_WEST;
    assert(SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_WEST, true)); events();
    assert(state(false).buttons & 1);
    input_cappy_rumble(0.5f, 0.25f); input_cappy_service(mario);
    assert(rumble_l >= 32767 && rumble_l <= 32768 && rumble_h >= 16383 && rumble_h <= 16384);
    SDL_CloseJoystick(j2);
    assert(SDL_DetachVirtualJoystick(cappy)); events(); input_cappy_service(mario);
    assert(!input_cappy_id());
    assert(input_cappy_assigned()); // Mario remains reserved while Cappy reconnects
    State disconnected = {0}; assert(!input_cappy_state(&disconnected, false));

    // Reconnect a uniquely identifiable controller, but do not guess when two match.
    cappy = attach(2); events(); input_cappy_service(mario);
    assert(input_cappy_id() == cappy);
    assert(!state(false).buttons);
    assert(SDL_DetachVirtualJoystick(cappy)); events(); input_cappy_service(mario);
    SDL_JoystickID same1 = attach(2), same2 = attach(2); events(); input_cappy_service(mario);
    assert(!input_cappy_id());
    input_cappy_assign(same2); input_cappy_service(mario); assert(input_cappy_id() == same2);
    input_cappy_assign(0); input_cappy_service(mario); assert(!input_cappy_id());
    assert(!input_cappy_assigned());
    input_cappy_service(mario); assert(!input_cappy_id()); // disabling also cancels reconnect
    assert(SDL_GamepadConnected(p1));
    SDL_CloseGamepad(p1);
    SDL_DetachVirtualJoystick(mario); SDL_DetachVirtualJoystick(same1); SDL_DetachVirtualJoystick(same2);
    SDL_Quit();
    puts("PASS: independent SDL input, bindings, directions, menu capture, short presses, rumble, reconnect and ambiguity");
    return 0;
}
