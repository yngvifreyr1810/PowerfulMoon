#pragma once
#include "vk.h"

typedef struct {
    VkImage color, depth, motion, output;
    VkFormat color_format;
    uint32_t w, h, ow, oh;
    float jitter[2], dt_ms, near_z, far_z, fov_y, sharpness;
    bool reset, ml;
} Fsr3Frame;
typedef struct {
    VkImage depth, motion;
    uint32_t w, h;
    float jitter[2], dt_ms, near_z, far_z, fov_y;
    bool reset;
} FgFrame;
#ifdef _WIN32
void vk_fsr3_init(void);
bool vk_fsr3_available(void);
bool vk_fsr4_available(void);
void vk_fsr3_release(void); // caller must wait for the last dispatch's submission
bool vk_fsr3_dispatch(VkCommandBuffer cb, const Fsr3Frame *frame);

// FSR frame generation (AMD's frame generation swapchain, Vulkan): one generated frame between each two the game
// renders. Its swapchain replaces ours (device.c routes the swapchain calls here while it is active); each rendered
// frame's depth and motion vectors are handed over by the temporal upscaler (dlss_probe.c).
bool vk_fg_wanted(void);     // the setting on, an upscaler on, the runtime and queues there
bool vk_fg_active(void);     // our swapchain is the frame generation one
VkResult vk_fg_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc);  // create, or recreate in place
void vk_fg_teardown(VkSwapchainKHR *sc);  // back to a plain swapchain (*sc: none; the caller creates one)
VkResult vk_fg_get_images(VkSwapchainKHR sc, uint32_t *n, VkImage *images);
VkResult vk_fg_acquire(VkSwapchainKHR sc, VkSemaphore sem, uint32_t *image);
VkResult vk_fg_present(VkQueue q, const VkPresentInfoKHR *pi);  // (queue lock held)
void vk_fg_prepare(VkCommandBuffer cb, const FgFrame *f);
void vk_fg_wait(void);  // its presents done (before vkDeviceWaitIdle)
#else  // (AMD's runtimes are Windows DLLs: elsewhere FSR 3.1 / 4 and frame generation are never available)
static inline bool vk_fsr3_available(void) { return false; }
static inline bool vk_fsr4_available(void) { return false; }
static inline void vk_fsr3_release(void) {}
static inline bool vk_fsr3_dispatch(VkCommandBuffer cb, const Fsr3Frame *f) { (void)cb, (void)f; return false; }
static inline bool vk_fg_active(void) { return false; }
static inline void vk_fg_prepare(VkCommandBuffer cb, const FgFrame *f) { (void)cb, (void)f; }
#endif
