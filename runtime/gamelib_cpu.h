// The CPU the game code is compiled for on this machine (runtime/gamelib.c): the build's own target (x86-64-v3:
// AVX2, FMA, BMI2) or, on an older x86-64 CPU, what it has. The library's id carries it (gamelib_id: GAMELIB_ID, plus
// -avx / -v2 / -x64 below v3).
// (Not in gamelib.h: that header is part of GAMELIB_ID's hash - a change there makes every player compile the game
// code again.)
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum { GAMELIB_CPU_V3 = 0, GAMELIB_CPU_AVX = 1, GAMELIB_CPU_V2 = 2, GAMELIB_CPU_X64 = 3 };
int gamelib_cpu_level(void);
const char *gamelib_cpu_march(void);  // -march for it; NULL: the build's own
const char *gamelib_cpu_name(void);
const char *gamelib_id(void);

#ifdef __cplusplus
}
#endif
