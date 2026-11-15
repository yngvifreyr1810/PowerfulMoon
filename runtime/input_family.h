#pragma once
#include <SDL3/SDL.h>
// 0 Nintendo/other, 1 Xbox, 2 PlayStation. Shared by both player assignments.
static inline int input_controller_family(SDL_Gamepad *pad) {
    if (!pad) return 0;
    switch (SDL_GetGamepadType(pad)) {
    case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: return 1;
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return 2;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return 0;
    default:
        switch (SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_SOUTH)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return 1;
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return 2;
        default: return 0;
        }
    }
}
