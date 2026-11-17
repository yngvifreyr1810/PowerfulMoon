// AMD's official Vulkan runtime, loaded optionally. SDK headers: third_party/fsr3_sdk.
#include "fsr3_sdk.h"
#ifdef _WIN32
#include <math.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include "../../../third_party/fsr3_sdk/api/include/vk/ffx_api_vk.h"
#include "../../../third_party/fsr3_sdk/upscalers/include/ffx_upscale.h"
#include "../../../third_party/fsr3_sdk/framegeneration/include/ffx_framegeneration.h"
typedef struct VkQueueInfoFFXAPI VkQueueInfoFFXAPI;  // (the SDK header names it the C++ way)
#include "../../../third_party/fsr3_sdk/framegeneration/include/vk/ffx_api_framegeneration_vk.h"
#include "../../settings.h"

static struct {
    HMODULE loader, runtime;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    PfnFfxConfigure configure;
    ffxContext context;
    uint64_t version[2];
    bool ml;
    uint32_t w, h, ow, oh;
} fsr;
static atomic_bool ready[2];

bool vk_fsr3_available(void) { return atomic_load_explicit(&ready[0], memory_order_relaxed); }
bool vk_fsr4_available(void) { return atomic_load_explicit(&ready[1], memory_order_relaxed); }

void vk_fsr3_init(void) {
    wchar_t path[2048];
    DWORD len = GetModuleFileNameW(NULL, path, sizeof path / sizeof *path);
    if (!len || len >= sizeof path / sizeof *path) return;
    wchar_t *name = wcsrchr(path, L'\\');
    if (!name || name - path > 1900) return;
    wcscpy(name + 1, L"amd_fidelityfx_upscaler_vk.dll");
    fsr.runtime = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!fsr.runtime) { LOG("[fsr3] optional AMD Vulkan runtime absent\n"); return; }
    wcscpy(name + 1, L"amd_fidelityfx_loader_vk.dll");
    fsr.loader = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!fsr.loader) { LOG("[fsr3] AMD Vulkan loader unavailable\n"); return; }
    fsr.create = (void *)GetProcAddress(fsr.loader, "ffxCreateContext");
    fsr.destroy = (void *)GetProcAddress(fsr.loader, "ffxDestroyContext");
    fsr.query = (void *)GetProcAddress(fsr.loader, "ffxQuery");
    fsr.dispatch = (void *)GetProcAddress(fsr.loader, "ffxDispatch");
    fsr.configure = (void *)GetProcAddress(fsr.loader, "ffxConfigure");
    if (!fsr.create || !fsr.destroy || !fsr.query || !fsr.dispatch) return;
    VkDeviceContext device = {g_vk.dev, g_vk.phys, vkGetDeviceProcAddr};
    uint64_t count = 16, ids[16] = {0};
    const char *names[16] = {0};
    struct ffxQueryDescGetVersions versions = {.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS,
        .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, .device = &device,
        .outputCount = &count, .versionIds = ids, .versionNames = names};
    ffxReturnCode_t r = fsr.query(NULL, &versions.header);
    if (r) { LOG("[fsr3] provider query failed: %u\n", r); return; }
    if (count > 16) count = 16;
    for (uint64_t i = 0; i < count; i++) {
        LOG("[fsr-sdk] provider %llu: %s\n", (unsigned long long)ids[i], names[i] ? names[i] : "unknown");
        unsigned major = 0, minor = 0;
        if (names[i]) sscanf(names[i], "%u.%u", &major, &minor);
        if (major == 3 && minor == 1 && !fsr.version[0]) fsr.version[0] = ids[i];
        if (major == 4 && !fsr.version[1]) fsr.version[1] = ids[i];
    }
    atomic_store_explicit(&ready[0], fsr.version[0] != 0, memory_order_relaxed);
    atomic_store_explicit(&ready[1], fsr.version[1] != 0, memory_order_relaxed);
    if (!fsr.version[1]) LOG("[fsr4] no ML provider supports this GPU; FSR 3.1 fallback is %s\n", fsr.version[0] ? "available" : "unavailable");
}

void vk_fsr3_release(void) {
    if (fsr.context) fsr.destroy(&fsr.context, NULL);
    fsr.context = NULL;
    fsr.w = fsr.h = fsr.ow = fsr.oh = 0;
}

static uint32_t surface_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_D32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

static struct FfxApiResource resource(VkImage image, VkFormat format, uint32_t w, uint32_t h, bool output) {
    return (struct FfxApiResource){.resource = (void *)(uintptr_t)image,
        .description = {.type = FFX_API_RESOURCE_TYPE_TEXTURE2D, .format = surface_format(format),
            .width = w, .height = h, .depth = 1, .mipCount = 1,
            .usage = output ? FFX_API_RESOURCE_USAGE_UAV : format == VK_FORMAT_D32_SFLOAT ? FFX_API_RESOURCE_USAGE_DEPTHTARGET : 0},
        .state = output ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ};
}

static void input_layouts(VkCommandBuffer cb, const Fsr3Frame *f, bool reading) {
    VkImageMemoryBarrier2 b[3] = {0};
    VkImage images[3] = {f->color, f->depth, f->motion};
    for (int i = 0; i < 3; i++) b[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = reading ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = reading ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = images[i], .subresourceRange = {i == 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 3, .pImageMemoryBarriers = b};
    vkCmdPipelineBarrier2(cb, &d);
}

bool vk_fsr3_dispatch(VkCommandBuffer cb, const Fsr3Frame *f) {
    bool ml = f->ml && vk_fsr4_available();
    if (!ml && !vk_fsr3_available()) return false;
    if (!fsr.context) {
        struct ffxCreateBackendVKDesc backend = {.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK,
            .deviceContext = {g_vk.dev, g_vk.phys, vkGetDeviceProcAddr}};
        struct ffxOverrideVersion version = {.header = {FFX_API_DESC_TYPE_OVERRIDE_VERSION, &backend.header}, .versionId = fsr.version[ml]};
        struct ffxCreateContextDescUpscaleVersion api = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, &version.header}, .version = FFX_UPSCALER_VERSION};
        struct ffxCreateContextDescUpscale desc = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, ml ? &api.header : &version.header},
            .flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE,
            .maxRenderSize = {f->w, f->h}, .maxUpscaleSize = {f->ow, f->oh}};
        ffxReturnCode_t r = fsr.create(&fsr.context, &desc.header, NULL);
        if (r) {
            LOG("[fsr%d] context creation failed: %u\n", ml ? 4 : 3, r);
            atomic_store(&ready[ml], false);
            vk_fsr3_release();
            if (ml && vk_fsr3_available()) return vk_fsr3_dispatch(cb, f);
            return false;
        }
        fsr.ml = ml;
        fsr.w = f->w; fsr.h = f->h; fsr.ow = f->ow; fsr.oh = f->oh;
        struct ffxQueryGetProviderVersion provider = {.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION};
        fsr.query(&fsr.context, &provider.header);
        LOG("[fsr%d] context created: %ux%u -> %ux%u, provider %s%s\n", ml ? 4 : 3, f->w, f->h, f->ow, f->oh,
            provider.versionName ? provider.versionName : "unknown", f->ml && !ml ? " (FSR 4 unavailable: analytical fallback)" : "");
    }
    if (fsr.w != f->w || fsr.h != f->h || fsr.ow != f->ow || fsr.oh != f->oh) return false;
    struct ffxDispatchDescUpscale desc = {.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE, .commandList = cb,
        .color = resource(f->color, f->color_format, f->w, f->h, false),
        .depth = resource(f->depth, VK_FORMAT_D32_SFLOAT, f->w, f->h, false),
        .motionVectors = resource(f->motion, VK_FORMAT_R16G16_SFLOAT, f->w, f->h, false),
        .output = resource(f->output, VK_FORMAT_R16G16B16A16_SFLOAT, f->ow, f->oh, true),
        .jitterOffset = {f->jitter[0], f->jitter[1]}, .motionVectorScale = {1, 1},
        .renderSize = {f->w, f->h}, .upscaleSize = {f->ow, f->oh},
        .enableSharpening = f->sharpness > 0, .sharpness = f->sharpness,
        .frameTimeDelta = f->dt_ms, .preExposure = 1, .reset = f->reset,
        .cameraNear = f->near_z, .cameraFar = f->far_z, .cameraFovAngleVertical = f->fov_y, .viewSpaceToMetersFactor = 0.01f,
        .flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB};
    input_layouts(cb, f, true);
    ffxReturnCode_t r = fsr.dispatch(&fsr.context, &desc.header);
    input_layouts(cb, f, false);
    if (r) { LOG("[fsr%d] dispatch failed: %u\n", fsr.ml ? 4 : 3, r); atomic_store(&ready[fsr.ml], false); return false; }
    return true;
}

// ================================================================== frame generation
// AMD's frame generation swapchain (amd_fidelityfx_framegeneration_vk.dll, next to the loader): it owns the real
// swapchain and a presenter of its own (threads, and the queues device.c set aside: g_vk.fgq), and puts a generated
// frame between each two we present, paced evenly. Each frame we present must come with its depth and motion vectors
// (vk_fg_prepare, from the temporal upscaler's inputs) under the same frame ID as its configuration (vk_fg_present);
// a frame without them (menus, loading, no 3D scene found) is presented with generation off.
// The game still renders 60 FPS: with VSync on, 120 on a 120 Hz+ display. ODYSSEY_FG=0/1 overrides the setting.
static struct {
    ffxContext sc_ctx, fg_ctx;          // the swapchain context, the frame generation context
    VkSwapchainKHR sc;                  // the frame generation swapchain
    struct ffxQueryDescSwapchainReplacementFunctionsVK fn;
    uint32_t dw, dh, rw, rh;            // the frame generation context's display / max render size
    uint64_t id, prepared;              // this frame's ID; the last one prepared (+1: 0 = none)
    uint64_t st_t0, st_id0, st_shown0, st_gen0, st_gen;  // ([fg] stats: from the swapchain's start)
    bool failed;
} fg;

static int fg_env(void) {
    static int v = -2;
    if (v == -2) { const char *e = getenv("ODYSSEY_FG"); v = e && *e ? atoi(e) != 0 : -1; }
    return v;
}
bool vk_fg_wanted(void) {
    int e = fg_env();
    // (not with the 90 / 120 FPS modes or 120 FPS interpolation: those make the extra frames themselves)
    bool want = e >= 0 ? e : g_settings.frame_gen == FG_FSR && !g_settings.temporal_off && settings_game_hz() == 60 &&
                             !g_settings.frame_interp;
    return want && !fg.failed && fsr.loader && fsr.configure && g_vk.fgfam[0] != UINT32_MAX && g_vk.fgfam[1] != UINT32_MAX;
}
bool vk_fg_active(void) { return fg.sc_ctx != NULL; }

static void fg_release_context(void) {
    if (fg.fg_ctx) fsr.destroy(&fg.fg_ctx, NULL);
    fg.fg_ctx = NULL;
    fg.dw = fg.dh = fg.rw = fg.rh = 0;
}

VkResult vk_fg_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc) {
    if (fg.sc_ctx) {  // (resize, VSync: the frame generation swapchain is remade in place)
        VkSwapchainKHR out = VK_NULL_HANDLE;
        VkSwapchainCreateInfoKHR c = *ci;
        c.oldSwapchain = VK_NULL_HANDLE;  // (as AMD's own framework does: the wrapper stays, the real one inside is remade)
        VkResult r = fg.fn.pOutCreateSwapchainFFXAPI(g_vk.dev, &c, NULL, &out, (void *)fg.sc);
        if (r == VK_SUCCESS && out) fg.sc = out;
        LOG("[fg] frame generation swapchain remade: %ux%u (%d)\n", ci->imageExtent.width, ci->imageExtent.height, r);
        *sc = fg.sc;
        fg_release_context();  // (the display size may have changed: made again at the next prepare)
        return r;
    }
    VkQueueInfoFFXAPI none = {0};
    struct ffxCreateContextDescFrameGenerationSwapChainVersionVK ver = {
        .header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK, NULL},
        .version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION};
    struct ffxCreateContextDescFrameGenerationSwapChainVK d = {
        .header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK, &ver.header},
        .physicalDevice = g_vk.phys, .device = g_vk.dev, .swapchain = sc, .createInfo = *ci,
        .gameQueue = {g_vk.queue, g_vk.qfam, NULL},
        .presentQueue = {g_vk.fgq[0], g_vk.fgfam[0], NULL},
        .imageAcquireQueue = {g_vk.fgq[1], g_vk.fgfam[1], NULL},
        .asyncComputeQueue = none};
    if (g_vk.fgfam[2] != UINT32_MAX) d.asyncComputeQueue = (VkQueueInfoFFXAPI){g_vk.fgq[2], g_vk.fgfam[2], NULL};
    d.createInfo.oldSwapchain = *sc;
    ffxReturnCode_t r = fsr.create(&fg.sc_ctx, &d.header, NULL);
    if (r || !*sc) {
        LOG("[fg] frame generation swapchain failed: %u (frame generation off)\n", r);
        fg.sc_ctx = NULL;
        fg.failed = true;
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    fg.sc = *sc;
    fg.fn = (struct ffxQueryDescSwapchainReplacementFunctionsVK){.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK};
    fsr.query(&fg.sc_ctx, &fg.fn.header);
    fg.prepared = 0;
    fg.st_t0 = fg.st_shown0 = 0;
    LOG("[fg] frame generation swapchain: %ux%u\n", ci->imageExtent.width, ci->imageExtent.height);
    return VK_SUCCESS;
}

void vk_fg_wait(void) {  // (before a device wait idle: its presenter's queues quiet)
    if (!fg.sc_ctx) return;
    struct ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK w = {.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK};
    fsr.dispatch(&fg.sc_ctx, &w.header);
}

void vk_fg_teardown(VkSwapchainKHR *sc) {
    if (!fg.sc_ctx) return;
    struct ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK w = {.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK};
    fsr.dispatch(&fg.sc_ctx, &w.header);
    fg_release_context();
    fsr.destroy(&fg.sc_ctx, NULL);
    fg.sc_ctx = NULL;
    if (fg.sc) fg.fn.pOutDestroySwapchainFFXAPI(g_vk.dev, fg.sc, NULL, NULL);
    fg.sc = VK_NULL_HANDLE;
    *sc = VK_NULL_HANDLE;
    LOG("[fg] frame generation off\n");
}

VkResult vk_fg_get_images(VkSwapchainKHR sc, uint32_t *n, VkImage *images) { return fg.fn.pOutGetSwapchainImagesKHR(g_vk.dev, sc, n, images); }
VkResult vk_fg_acquire(VkSwapchainKHR sc, VkSemaphore sem, uint32_t *image) {
    return fg.fn.pOutAcquireNextImageKHR(g_vk.dev, sc, UINT64_MAX, sem, VK_NULL_HANDLE, image);
}

static ffxReturnCode_t fg_generate(ffxDispatchDescFrameGeneration *p, void *user) {
    (void)user;
    return fg.fg_ctx ? fsr.dispatch(&fg.fg_ctx, &p->header) : FFX_API_RETURN_ERROR;
}

static void fg_configure(bool on) {
    if (!fg.fg_ctx) return;
    struct ffxConfigureDescFrameGeneration c = {.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION,
        .swapChain = (void *)fg.sc, .frameGenerationCallback = fg_generate, .frameGenerationEnabled = on,
        .allowAsyncWorkloads = false, .generationRect = {0, 0, (int)fg.dw, (int)fg.dh}, .frameID = fg.id};
    ffxReturnCode_t r = fsr.configure(&fg.fg_ctx, &c.header);
    static int nlog;
    if (r && nlog++ < 5) LOG("[fg] configure failed: %u\n", r);
}

void vk_fg_prepare(VkCommandBuffer cb, const FgFrame *f) {
    if (!fg.sc_ctx || fg.failed) return;
    uint32_t dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
    if (fg.fg_ctx && (fg.dw != dw || fg.dh != dh || f->w > fg.rw || f->h > fg.rh)) fg_release_context();
    if (!fg.fg_ctx) {
        struct ffxCreateBackendVKDesc backend = {.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK,
            .deviceContext = {g_vk.dev, g_vk.phys, vkGetDeviceProcAddr}};
        struct ffxCreateContextDescFrameGenerationVersion ver = {
            .header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION, &backend.header}, .version = FFX_FRAMEGENERATION_VERSION};
        struct ffxCreateContextDescFrameGeneration d = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, &ver.header},
            .flags = 0, .displaySize = {dw, dh}, .maxRenderSize = {f->w, f->h}, .backBufferFormat = surface_format(g_vk.sc_format)};
        ffxReturnCode_t r = fsr.create(&fg.fg_ctx, &d.header, NULL);
        if (r) {
            LOG("[fg] frame generation context failed: %u\n", r);
            fg.fg_ctx = NULL;
            fg.failed = true;
            return;
        }
        fg.dw = dw, fg.dh = dh, fg.rw = f->w, fg.rh = f->h;
        struct ffxQueryGetProviderVersion provider = {.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION};
        fsr.query(&fg.fg_ctx, &provider.header);
        LOG("[fg] frame generation: render %ux%u, display %ux%u, provider %s\n", f->w, f->h, dw, dh,
            provider.versionName ? provider.versionName : "unknown");
    }
    fg_configure(true);
    struct ffxDispatchDescFrameGenerationPrepareV2 p = {.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2,
        .frameID = fg.id, .commandList = cb, .renderSize = {f->w, f->h}, .jitterOffset = {f->jitter[0], f->jitter[1]},
        .motionVectorScale = {1, 1}, .frameTimeDelta = f->dt_ms, .reset = f->reset, .cameraNear = f->near_z,
        .cameraFar = f->far_z, .cameraFovAngleVertical = f->fov_y, .viewSpaceToMetersFactor = 0.01f,
        .depth = resource(f->depth, VK_FORMAT_D32_SFLOAT, f->w, f->h, false),
        .motionVectors = resource(f->motion, VK_FORMAT_R16G16_SFLOAT, f->w, f->h, false)};
    Fsr3Frame layouts = {.color = f->motion, .depth = f->depth, .motion = f->motion};  // (the inputs read: shader-read layout)
    input_layouts(cb, &layouts, true);
    ffxReturnCode_t r = fsr.dispatch(&fg.fg_ctx, &p.header);
    input_layouts(cb, &layouts, false);
    if (r) { static int nlog; if (nlog++ < 5) LOG("[fg] prepare failed: %u\n", r); return; }
    fg.prepared = fg.id + 1;
}

VkResult vk_fg_present(VkQueue q, const VkPresentInfoKHR *pi) {
    if (fg.prepared != fg.id + 1) fg_configure(false);  // (no depth / motion for this frame: shown as it is)
    VkResult r = fg.fn.pOutQueuePresentKHR(q, pi);
    fg.id++;
    fg.st_gen += fg.prepared == fg.id;  // (frames presented with generation on)
    uint64_t now = plat_now_ns();
    if (!fg.st_t0) fg.st_t0 = now, fg.st_id0 = fg.id, fg.st_gen0 = fg.st_gen;
    if (now - fg.st_t0 >= 5000000000ull && fg.fn.pOutGetLastPresentCountFFXAPI) {  // (every 5 s: what reached the screen)
        uint64_t shown = fg.fn.pOutGetLastPresentCountFFXAPI(fg.sc);
        double s = (now - fg.st_t0) / 1e9, n = (double)(fg.id - fg.st_id0);
        LOG("[fg] %.1f s: %.1f FPS rendered (%.0f%% with frame generation), %.1f FPS presented\n", s, n / s,
            n > 0 ? 100.0 * (fg.st_gen - fg.st_gen0) / n : 0.0, fg.st_shown0 && shown >= fg.st_shown0 ? (shown - fg.st_shown0) / s : 0.0);
        fg.st_t0 = now, fg.st_id0 = fg.id, fg.st_shown0 = shown, fg.st_gen0 = fg.st_gen;
    }
    return r;
}
#endif
