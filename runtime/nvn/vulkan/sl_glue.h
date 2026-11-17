#pragma once
// NVIDIA Streamline (Windows): DLSS Frame Generation and Reflex on our Vulkan renderer, through Streamline's manual
// hooking - its instance / device / surface / swapchain functions in place of Vulkan's (sl.interposer.dll's proxies),
// each frame's depth and motion vectors tagged with the camera constants, and the latency markers. sl_glue.cpp
// (C++: Streamline's headers are). Its runtime DLLs ship next to the exe (tools/fetch_upscalers.sh).
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t w, h;
    VkImageUsageFlags usage;
} SlImage;

typedef struct {
    SlImage depth, mvec;
    float view_to_clip[16], clip_to_prev_clip[16];  // (row major, row vectors; no jitter)
    float jitter[2];                                  // (pixels)
    float near_z, far_z, fov_y, aspect;
    bool reset;
} SlFrame;

#ifdef _WIN32
bool vk_sl_init(void);     // before the instance: the setting wants DLSS-G or Reflex, the runtime is there; slInit
bool vk_sl_active(void);   // Streamline initialised (its proxies in use)
PFN_vkVoidFunction vk_sl_instance_proc(VkInstance inst, const char *name);  // (NULL: not active)
PFN_vkVoidFunction vk_sl_device_proc(VkDevice dev, const char *name);
void vk_sl_device_ready(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t qfam);  // what it supports
bool vk_sl_dlssg_available(void);
bool vk_sl_reflex_available(void);
void vk_sl_frame(uint64_t frame, VkCommandBuffer cb, const SlFrame *f);  // a frame with a 3D scene: tags + constants
void vk_sl_marker(uint64_t frame, int marker);  // (PCL markers: 0 sim start .. 5 present end)
void vk_sl_frame_start(uint64_t frame);         // Reflex sleep + simulation / render markers
void vk_sl_before_present(uint64_t frame, uint32_t w, uint32_t h);  // DLSS-G options for this frame
void vk_sl_shutdown(void);
void vk_sl_swapchain(uint32_t images, VkFormat format);  // (its back buffers: DLSS-G options)
#else
static inline bool vk_sl_active(void) { return false; }
#endif

#ifdef __cplusplus
}
#endif
