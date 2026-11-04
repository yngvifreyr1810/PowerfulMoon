// Link's Awakening's feature hooks in the Metal backend (texpatch.h): none of them applies to Odyssey, so each answers
// "nothing to do" - the backend then draws as the game asks. (macOS only: the Vulkan backend never had them.)
#include <stdbool.h>
#include <stdint.h>
#include "texpatch.h"

int texpatch_identify(const NvnTexture *t) { (void)t; return -1; }
int texpatch_bottle_kind(int id) { (void)id; return 0; }
bool texpatch_is_item_cursor(int id) { (void)id; return false; }
uint32_t texpatch_generation(void) { return 0; }
void texpatch_apply(int id, uint32_t lv, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row) {
    (void)id, (void)lv, (void)blocks, (void)w, (void)h, (void)row;
}
int texpatch_heart_kind(int id) { (void)id; return 0; }
void texpatch_heart_tint(int kind, float tint[8]) { (void)kind, (void)tint; }

const char *game_level_name(void) { return ""; }
void warp_frame(uint64_t frame) { (void)frame; }
void save_editor_frame(uint64_t frame) { (void)frame; }

bool widescreen_allowed(void) { return false; }
void widescreen_frame_end(uint64_t frame) { (void)frame; }
void widescreen_set_factor(float f) { (void)f; }
void widescreen_backdrop_seen(uint64_t hash) { (void)hash; }
void widescreen_scene_ubo(const float *u, uint64_t size) { (void)u, (void)size; }
float widescreen_effect_stretch(uint64_t fs_hash) { (void)fs_hash; return 1.0f; }
// the whole image, where output_scale put it
void widescreen_present_crop(uint64_t frame, double shape, float *crop, double *vx, double *vy, double *vw, double *vh) {
    (void)frame, (void)shape, (void)vx, (void)vy, (void)vw, (void)vh;
    crop[0] = 0, crop[1] = 0, crop[2] = 1, crop[3] = 1;
}

bool mirror_draw(bool composite) { (void)composite; return false; }
bool pets_hide_item_cursor(void) { return false; }
bool pot_level_active(void) { return false; }
bool pot_glow_active(void) { return false; }
void pot_item_draw(float *m) { (void)m; }
void pot_probe(const float *m, bool tintable, uint64_t mesh, uint32_t inst) { (void)m, (void)tintable, (void)mesh, (void)inst; }
bool pot_tint(const float *m, float tint[8], uint64_t mesh) { (void)m, (void)tint, (void)mesh; return false; }
bool pot_instance_gold(const float *m) { (void)m; return false; }
void pot_gold_tint(float tint[8]) { (void)tint; }
void escape_bottle_tint(int kind, const float *ui, uint32_t ui_size, float tint[8]) {
    (void)kind, (void)ui, (void)ui_size, (void)tint;
}
