// SPDX-License-Identifier: MIT
// Controller motion / vibration ABI adapter, implemented for this fork.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    int64_t delta_ns, sampling;
    float acceleration[3], angular_velocity[3], angle[3], direction[9];
    uint32_t attributes, reserved;
} PMMotionState;
typedef struct { float low_amplitude, low_frequency, high_amplitude, high_frequency; } PMVibration;

struct SDL_Gamepad;
union SDL_Event;
void pm_controller_attach(struct SDL_Gamepad *pad);
void pm_controller_detach(void);
void pm_controller_event(const union SDL_Event *event);
void pm_controller_tick(void);
int pm_motion_read(PMMotionState *out, int count);
bool pm_motion_at_rest(void);
bool pm_motion_available(void);
void pm_motion_reset(void);
void pm_vibration_send(uint32_t handle, const PMVibration *value);
PMVibration pm_vibration_actual(uint32_t handle);
void pm_vibration_permit(bool permit);
bool pm_vibration_permitted(void);
void pm_vibration_test(void);
#ifdef __cplusplus
}
#endif
