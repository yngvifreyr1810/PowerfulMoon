#pragma once
#include <stdbool.h>
#include <stdint.h>
union SDL_Event;
#ifdef __cplusplus
extern "C" {
#endif
// SDL instance IDs are session-local. Requests are applied on the UI/event thread.
uint32_t input_primary_id(void);
uint32_t input_cappy_id(void);
bool input_cappy_assigned(void); // includes a disconnected controller awaiting reconnect
int input_cappy_family(void);
void input_cappy_assign(uint32_t id); // zero disconnects player two
void input_cappy_service(uint32_t primary_id);
bool input_cappy_event(const union SDL_Event *event);
void input_cappy_tick_begin(void);
void input_cappy_tick_end(void);
void input_cappy_reset_presses(void);
bool input_cappy_state(void *state, bool menu);
void input_cappy_rumble(float low, float high);
bool input_cappy_test_device(uint32_t id); // only process-local test devices
#ifdef __cplusplus
}
#endif
