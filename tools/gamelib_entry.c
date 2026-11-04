// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the function
// tables, found by name when the app loads the library. Everything else in it stays hidden.
#include "gamelib.h"

// GAMELIB_MODULES (-D, from the Makefile): X(name) for each module in generated/, in load order.
#ifndef GAMELIB_MODULES
#define GAMELIB_MODULES X(main) X(sdk)
#endif

#define X(m) extern const uint64_t m##_fn_addrs[], m##_fn_count; extern const GuestFn m##_fn_ptrs[];
GAMELIB_MODULES
#undef X

GAMELIB_EXPORT const GameLibTables *odyssey_game_tables(void) {
#define X(m) {#m, m##_fn_addrs, m##_fn_ptrs, &m##_fn_count},
    static const GameLibModule mods[] = {GAMELIB_MODULES};
#undef X
    _Static_assert(sizeof mods / sizeof mods[0] <= GAMELIB_MAX_MODULES, "too many modules");
    static GameLibTables t = {GAMELIB_ABI, GAMELIB_ID, sizeof mods / sizeof mods[0]};
    for (uint32_t i = 0; i < t.nmodules; i++) t.modules[i] = mods[i];
    return &t;
}
