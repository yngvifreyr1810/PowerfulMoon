// Hooks the Metal backend (metal.m, renderer.m) still calls from Link's Awakening's features - texture patches, the
// widescreen view, pot glow, mirror mode. Odyssey has none of them: runtime/nvn/metal_stubs.m answers "nothing to do".
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "nvn.h"
int texpatch_identify(const NvnTexture *t);
int texpatch_bottle_kind(int id);
bool texpatch_is_item_cursor(int id);
uint32_t texpatch_generation(void);
void texpatch_apply(int id, uint32_t lv, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row);
int texpatch_heart_kind(int id);
void texpatch_heart_tint(int kind, float tint[8]);
