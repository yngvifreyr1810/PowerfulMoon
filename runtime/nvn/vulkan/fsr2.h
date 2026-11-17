#pragma once
#include "vk.h"

// AMD FidelityFX Super Resolution 2.2 (third_party/fsr2: AMD's GLSL shaders, MIT), run by our own host code - the
// temporal upscaler for Linux / the Steam Deck, where AMD's prebuilt FSR 3.1 / 4 runtimes (Windows DLLs) can't go.
// Inputs as the other temporal upscalers get them (dlss_probe.c): the scene color, depth and pixel motion vectors at
// the render size, the jitter in pixels; output: an R16G16B16A16_SFLOAT storage image at the display size.
typedef struct {
    VkImageView color_view, depth_view, motion_view, output_view;
    VkImage output;
    uint32_t w, h, ow, oh;
    float jitter[2], dt_ms, near_z, far_z, fov_y, sharpness;  // (sharpness 0..1: RCAS when > 0)
    bool reset;
} Fsr2Frame;

bool vk_fsr2_available(void);  // (the shaders found and built; checked once, at the first dispatch)
bool vk_fsr2_dispatch(VkCommandBuffer cb, const Fsr2Frame *f);
void vk_fsr2_release(void);  // (its images: the caller has waited for its last dispatch)
