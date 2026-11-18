// Vulkan device, window, swapchain and presentation (counterpart of the window/present half of metal.m).
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>  // (NVIDIA Streamline: the window surface through its proxy)
#endif
#include <SDL3/SDL_vulkan.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../paths.h"
#include "../../odyssey_pose.h"
#include "../../settings.h"
#include "../../ui/menu.h"
#include "../../ui/events.h"
#include "../../diagnostics.h"
#include "../../updater.h"
#include "dlss_probe.h"
#include "xess_sdk.h"
#include "fsr3_sdk.h"
#include "sl_glue.h"
#ifdef _WIN32
#define SL_TEST_EXIT() vk_sl_shutdown()  // (tests exit at once: Streamline's threads stopped first)
#else
#define SL_TEST_EXIT() ((void)0)
#endif
#include "shaders.h"
#include "vk.h"
#include "../../version.h"
#include "../interp.h"
#include "../output_scale.h"
#include "../../ui/performance.h"
#include "../renderer.h"

VkG g_vk;
static _Atomic bool g_no_present;  // (quitting: no present to a window going away - ui_run)
bool vk_image_share_small(VkImageCreateInfo *ci) {
    static uint32_t fams[2];
    if (g_vk.cqfam == UINT32_MAX || g_vk.cqfam == g_vk.qfam || ci->samples != VK_SAMPLE_COUNT_1_BIT) return false;
    if ((uint64_t)ci->extent.width * ci->extent.height * ci->extent.depth * ci->arrayLayers > 65536) return false;  // (e.g. 32x32x32 lookup tables)
    fams[0] = g_vk.qfam, fams[1] = g_vk.cqfam;
    ci->sharingMode = VK_SHARING_MODE_CONCURRENT;
    ci->queueFamilyIndexCount = 2;
    ci->pQueueFamilyIndices = fams;
    return true;
}
void vk_buffer_share(VkBufferCreateInfo *bi) {
    static uint32_t fams[2];
    if (g_vk.cqfam == UINT32_MAX || g_vk.cqfam == g_vk.qfam) return;
    fams[0] = g_vk.qfam, fams[1] = g_vk.cqfam;
    bi->sharingMode = VK_SHARING_MODE_CONCURRENT;
    bi->queueFamilyIndexCount = 2;
    bi->pQueueFamilyIndices = fams;
}
static SDL_Window *window;
static atomic_bool ui_ready;
static VkDebugUtilsMessengerEXT messenger;

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    static int n;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && n++ < 500) LOG("[vk] %s\n", d->pMessage);
    return VK_FALSE;
}

// ODYSSEY_VK_VALIDATION=1: Khronos validation layer (MSYS2 package vulkan-validation-layers)
static void create_instance(void) {
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    const char *ext[32];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
#ifdef _WIN32
    vk_dlss_probe_instance(ext, &n_ext, SDL_arraysize(ext) - 1);
    vk_xess_instance_extensions(ext, &n_ext, SDL_arraysize(ext) - 1);
#endif
    bool validate = getenv("ODYSSEY_VK_VALIDATION") != NULL;
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "SuperOdysseyRecomp",
                             .pEngineName = "SuperOdysseyRecomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    PFN_vkCreateInstance create = vkCreateInstance;
#ifdef _WIN32
    if (!g_vk.headless && vk_sl_init()) {  // (NVIDIA Streamline: its proxy sets up what DLSS-G / Reflex need)
        PFN_vkCreateInstance p = (PFN_vkCreateInstance)vk_sl_instance_proc(VK_NULL_HANDLE, "vkCreateInstance");
        if (p) create = p;
        bool have = false;  // (its plugins name their objects: VK_EXT_debug_utils, else a null call in sl.common)
        for (uint32_t i = 0; i < ci.enabledExtensionCount; i++) have |= !strcmp(ext[i], "VK_EXT_debug_utils");
        if (!have && !validate && ci.enabledExtensionCount < SDL_arraysize(ext)) ext[ci.enabledExtensionCount++] = "VK_EXT_debug_utils";
    }
#endif
    VkResult r = create(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = create(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.3 driver installed?)\n", r);
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.inst, &mi, NULL, &messenger);
    }
}

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

#ifdef _WIN32
static void *vk_dlss_probe_thread(void *arg) { (void)arg; vk_dlss_probe_run(); return NULL; }
#endif

// Discrete GPU first (ODYSSEY_GPU=<index> picks one), needs Vulkan 1.3 and a queue that can present.
static void pick_device(void) {
    uint32_t n = 0;
    PFN_vkEnumeratePhysicalDevices enumerate = vkEnumeratePhysicalDevices;
#ifdef _WIN32
    if (vk_sl_active()) {  // (NVIDIA Streamline learns each GPU's instance from its own vkEnumeratePhysicalDevices)
        PFN_vkEnumeratePhysicalDevices p = (PFN_vkEnumeratePhysicalDevices)vk_sl_instance_proc(g_vk.inst, "vkEnumeratePhysicalDevices");
        if (p) enumerate = p;
    }
#endif
    enumerate(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    enumerate(g_vk.inst, &n, pd);
    int want = getenv("ODYSSEY_GPU") ? atoi(getenv("ODYSSEY_GPU")) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if ((int)i == want) score = 100;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) FATAL("no Vulkan 1.3 device\n");
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 description = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &description);
    char info[1024];
    snprintf(info, sizeof info, "%s; %s; %s; driver version %#x; vendor %#x device %#x; Vulkan %u.%u.%u",
             g_vk.props.deviceName, driver.driverName, driver.driverInfo, g_vk.props.driverVersion,
             g_vk.props.vendorID, g_vk.props.deviceID, VK_API_VERSION_MAJOR(g_vk.props.apiVersion),
             VK_API_VERSION_MINOR(g_vk.props.apiVersion), VK_API_VERSION_PATCH(g_vk.props.apiVersion));
    diagnostics_gpu(info);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) { g_vk.qfam = i; break; }
    }
    // (Linux: a compute-only family for exec.c's compute queue; ODYSSEY_VK_COMPUTE_QUEUE=0: none)
    g_vk.cqfam = UINT32_MAX;
#ifdef __linux__
    {
        const char *e = getenv("ODYSSEY_VK_COMPUTE_QUEUE");
        if (!e || !*e || atoi(e))
            for (uint32_t i = 0; i < nq; i++)
                if ((qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qp[i].timestampValidBits) {
                    g_vk.cqfam = i;
                    break;
                }
    }
#endif
    // (Windows: three more queues for FSR frame generation's presenter, which submits and presents from threads of
    // its own: present (a family that can present), image acquire (any), async compute (compute). Each a queue no
    // one else uses; the game's is family qfam index 0.)
    for (int k = 0; k < 3; k++) g_vk.fgfam[k] = UINT32_MAX;
#ifdef _WIN32
    if (!vk_sl_active()) {  // (not with NVIDIA Streamline: it adds queues of its own; FSR frame generation is off then)
        uint32_t used[32] = {0};
        if (g_vk.qfam < 32) used[g_vk.qfam] = 1;
        if (g_vk.cqfam < 32) used[g_vk.cqfam]++;
        for (int k = 0; k < 3; k++)
            for (int pass = 0; pass < 2 && g_vk.fgfam[k] == UINT32_MAX; pass++)
                for (uint32_t i = 0; i < nq && i < 32; i++) {
                    VkBool32 present = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
                    VkQueueFlags fl = qp[i].queueFlags;
                    bool ok = k == 0 ? present && (fl & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))
                            : k == 2 ? (fl & VK_QUEUE_COMPUTE_BIT) != 0 : true;
                    // (first pass: a family of its own kind - present: the graphics one, acquire: transfer-only,
                    // async: compute-only; then any)
                    bool pref = k == 0 ? (fl & VK_QUEUE_GRAPHICS_BIT) != 0 : k == 1 ? !(fl & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
                              : !(fl & VK_QUEUE_GRAPHICS_BIT);
                    if (ok && (pass || pref) && used[i] < qp[i].queueCount) {
                        g_vk.fgfam[k] = i, g_vk.fgidx[k] = used[i]++;
                        break;
                    }
                }
        LOG("[vk] frame generation queues: present %d.%d, acquire %d.%d, async %d.%d\n", (int)g_vk.fgfam[0], (int)g_vk.fgidx[0],
            (int)g_vk.fgfam[1], (int)g_vk.fgidx[1], (int)g_vk.fgfam[2], (int)g_vk.fgidx[2]);
    }
#endif
    free(qp);
    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\n");
    LOG("[vk] using %s\n", g_vk.props.deviceName);
}

static bool g_fault_ext;
// After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), into the log.
void vk_device_fault_report(void) {
    static int once;
    if (once++) return;
    vk_checkpoints_report();
    if (!g_fault_ext) return;
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) return;
    VkDeviceFaultCountsEXT c = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(g_vk.dev, &c, NULL) != VK_SUCCESS) { LOG("[vk] device fault: no info\n"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (c.addressInfoCount > 16) c.addressInfoCount = 16;
    if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
    c.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend};
    if (get(g_vk.dev, &c, &info) != VK_SUCCESS && 0) return;
    LOG("[vk] device fault: %s\n", info.description);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (uint32_t i = 0; i < c.addressInfoCount; i++)
        LOG("[vk]   address %s %#llx (precision %#llx)\n", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < c.vendorInfoCount; i++)
        LOG("[vk]   vendor: %s (code %#llx data %#llx)\n", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
}

static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[32];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
#ifdef __linux__
    const char *placed_env = getenv("ODYSSEY_VK_PLACED_HEAP");
    if ((!placed_env || atoi(placed_env)) && has_ext(ep, ne, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME) &&
        has_ext(ep, ne, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &placed};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &f);
        VkPhysicalDeviceMapMemoryPlacedPropertiesEXT p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &props);
        g_vk.placed_heap = placed.memoryMapPlaced && placed.memoryUnmapReserve && p.minPlacedMemoryMapAlignment &&
            !(HEAP_BASE % p.minPlacedMemoryMapAlignment) && !(HEAP_MAX % p.minPlacedMemoryMapAlignment);
        g_vk.placed_align = p.minPlacedMemoryMapAlignment;
        if (g_vk.placed_heap) {
            ext[n_ext++] = VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME;
            ext[n_ext++] = VK_KHR_MAP_MEMORY_2_EXTENSION_NAME;
            placed.memoryMapRangePlaced = VK_FALSE;
        }
    }
#endif
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    g_fault_ext = fault;
    // ODYSSEY_VK_CHECKPOINTS=1 (NVIDIA): a marker before every draw / dispatch; on device loss the last ones the GPU
    // reached name the culprit (exec.c vk_checkpoint, vk_checkpoints_report)
    g_vk.checkpoints = getenv("ODYSSEY_VK_CHECKPOINTS") && has_ext(ep, ne, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    if (g_vk.checkpoints) ext[n_ext++] = VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME;
    else if (getenv("ODYSSEY_VK_CHECKPOINTS")) LOG("[vk] no VK_NV_device_diagnostic_checkpoints\n");
    // Pipeline libraries: the driver compiles a program's shaders once, a new combination of vertex layout, targets and
    // blending only links them (well under a millisecond instead of the 20-200 ms of a whole pipeline: the hitches when
    // something new shows up). Only with fast linking. ODYSSEY_NO_GPL=1: whole pipelines as before.
    VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gplp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gplf = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    if (has_ext(ep, ne, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) && has_ext(ep, ne, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
        !getenv("ODYSSEY_NO_GPL")) {
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &gplp};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &gplf};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
        g_vk.gpl = gplf.graphicsPipelineLibrary && gplp.graphicsPipelineLibraryFastLinking;
    }
    if (g_vk.gpl) {
        ext[n_ext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
    }
    LOG("[vk] pipeline libraries: %s\n", g_vk.gpl ? "on" : getenv("ODYSSEY_NO_GPL") ? "off (ODYSSEY_NO_GPL)" : "not supported");
    // maintenance5: a point drawn by a vertex shader that writes no point size is one pixel (undefined without it; Odyssey
    // draws a few point lists that way - the validation layer flagged them)
    VkPhysicalDeviceMaintenance5FeaturesKHR m5 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    if (has_ext(ep, ne, VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &m5};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
        m5.pNext = NULL;
        if (m5.maintenance5) ext[n_ext++] = VK_KHR_MAINTENANCE_5_EXTENSION_NAME;
    }
    // Pipeline binaries: a compiled shader library saved as data of our own, and created from that data in a later run
    // or in another process - no compile (exec.c lib_binary_*). ODYSSEY_NO_PBIN=1: off.
    VkPhysicalDevicePipelineBinaryFeaturesKHR pbf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR};
    if (g_vk.gpl && m5.maintenance5 && has_ext(ep, ne, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) && !getenv("ODYSSEY_NO_PBIN")) {
        VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &pbf};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
        pbf.pNext = NULL;
        if (pbf.pipelineBinaries) ext[n_ext++] = VK_KHR_PIPELINE_BINARY_EXTENSION_NAME;
    }
    g_vk.pbin = pbf.pipelineBinaries;
    LOG("[vk] pipeline binaries: %s\n", g_vk.pbin ? "on" : "off");
#ifdef _WIN32
    vk_dlss_probe_device(ep, ne, ext, &n_ext, SDL_arraysize(ext));
    vk_xess_device_extensions(ep, ne, ext, &n_ext, SDL_arraysize(ext));
    if (vk_sl_active()) {  // (NVIDIA Streamline looks some core 1.3 functions up by their extension names)
        static const char *const promoted[] = {"VK_KHR_synchronization2", "VK_KHR_copy_commands2", "VK_KHR_dynamic_rendering",
                                               "VK_KHR_create_renderpass2", "VK_KHR_maintenance3", "VK_KHR_get_memory_requirements2"};
        for (size_t k = 0; k < SDL_arraysize(promoted) && n_ext < SDL_arraysize(ext); k++) {
            bool has = false, there = false;
            for (uint32_t i = 0; i < n_ext; i++) has |= !strcmp(ext[i], promoted[k]);
            for (uint32_t i = 0; i < ne; i++) there |= !strcmp(ep[i].extensionName, promoted[k]);
            if (!has && there) ext[n_ext++] = promoted[k];
        }
    }
#endif
    // present timing (runtime/nvn/vulkan/pacing.c): when each image reached the screen
    VkPhysicalDevicePresentWaitFeaturesKHR pwf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    VkPhysicalDevicePresentIdFeaturesKHR pidf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, .pNext = &pwf};
    if (vk_pacing_want() && has_ext(ep, ne, VK_KHR_PRESENT_ID_EXTENSION_NAME) && has_ext(ep, ne, VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &pidf};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
        g_pw_enabled = pidf.presentId && pwf.presentWait;
        if (g_pw_enabled) {
            ext[n_ext++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
            ext[n_ext++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
        }
    }
    free(ep);

    // features the backend relies on (all present on current desktop drivers)
    VkPhysicalDeviceVulkan13Features have13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features have12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &have13};
    VkPhysicalDeviceVulkan11Features have11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &have12};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have11};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE, .shaderTerminateInvocation = VK_TRUE,
                                            .maintenance4 = VK_TRUE,
                                            .shaderIntegerDotProduct = have13.shaderIntegerDotProduct,
                                            .subgroupSizeControl = have13.subgroupSizeControl,
                                            .computeFullSubgroups = have13.computeFullSubgroups,
                                            // texel fetches outside the image return 0 on the Switch; in Vulkan they are
                                            // undefined without this (Odyssey's dynamic resolution: device lost)
                                            .robustImageAccess = have13.robustImageAccess,
                                            .pipelineCreationCacheControl = have13.pipelineCreationCacheControl,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .shaderFloat16 = have12.shaderFloat16,
                                            .shaderSubgroupExtendedTypes = have12.shaderSubgroupExtendedTypes,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = VK_TRUE, .samplerMirrorClampToEdge = VK_TRUE,
                                            .uniformAndStorageBuffer8BitAccess = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE,
                                            .scalarBlockLayout = VK_TRUE, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .uniformAndStorageBuffer16BitAccess = have11.uniformAndStorageBuffer16BitAccess,
                                            .storageBuffer16BitAccess = VK_TRUE, .pNext = &f12};
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                           .pNext = &f11};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fault ? (void *)&ff : (void *)&f11};
    if (g_vk.gpl) { gplf.pNext = f.pNext; f.pNext = &gplf; }  // (graphicsPipelineLibrary: true from the query)
    if (m5.maintenance5) { m5.pNext = f.pNext; f.pNext = &m5; }
    if (g_vk.pbin) { pbf.pNext = f.pNext; f.pNext = &pbf; }
    g_vk.cache_control = have13.pipelineCreationCacheControl;
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    g_vk.feat_aniso = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
    b->shaderInt64 = h->shaderInt64;
    b->shaderInt16 = h->shaderInt16;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    g_vk.features_depth_clamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    b->shaderClipDistance = h->shaderClipDistance;
    b->shaderStorageImageWriteWithoutFormat = h->shaderStorageImageWriteWithoutFormat;
    b->shaderStorageImageReadWithoutFormat = h->shaderStorageImageReadWithoutFormat;
    b->shaderStorageImageExtendedFormats = h->shaderStorageImageExtendedFormats;
#ifdef _WIN32
    vk_xess_enable_features(&f, &f11, &f12, &f13);
#endif
    g_vk.storage_write_without_format = b->shaderStorageImageWriteWithoutFormat;
    if (g_vk.placed_heap) { placed.pNext = f.pNext; f.pNext = &placed; }
    if (g_pw_enabled) { pidf.presentId = VK_TRUE, pwf.presentWait = VK_TRUE, pwf.pNext = f.pNext; f.pNext = &pidf; }
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
    static const float prios[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    VkDeviceQueueCreateInfo qis[5];
    uint32_t nqi = 0;
    {   // (each family once, with as many queues as asked of it: the game's, the compute queue, frame generation's)
        uint32_t fam[5] = {g_vk.qfam, g_vk.cqfam, g_vk.fgfam[0], g_vk.fgfam[1], g_vk.fgfam[2]};
        uint32_t idx[5] = {0, 0, g_vk.fgidx[0], g_vk.fgidx[1], g_vk.fgidx[2]};
        for (int k = 0; k < 5; k++) {
            if (fam[k] == UINT32_MAX) continue;
            uint32_t j = 0;
            while (j < nqi && qis[j].queueFamilyIndex != fam[k]) j++;
            if (j == nqi) qis[nqi++] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                                 .queueFamilyIndex = fam[k], .queueCount = 0, .pQueuePriorities = prios};
            if (qis[j].queueCount < idx[k] + 1) qis[j].queueCount = idx[k] + 1;
        }
    }
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f,
                             .queueCreateInfoCount = nqi,
                             .pQueueCreateInfos = qis, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    PFN_vkCreateDevice create_dev = vkCreateDevice;
#ifdef _WIN32
    if (vk_sl_active()) {
        PFN_vkCreateDevice p = (PFN_vkCreateDevice)vk_sl_instance_proc(g_vk.inst, "vkCreateDevice");
        if (p) create_dev = p;
    }
#endif
    VK_CHECK(create_dev(g_vk.phys, &di, NULL, &g_vk.dev));
#ifdef _WIN32
    vk_sl_device_ready(g_vk.inst, g_vk.phys, g_vk.dev, g_vk.qfam);
#endif
    vk_pacing_init();
#ifdef _WIN32
    vk_xess_device_created();
#endif
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    for (int k = 0; k < 3; k++) if (g_vk.fgfam[k] != UINT32_MAX) vkGetDeviceQueue(g_vk.dev, g_vk.fgfam[k], g_vk.fgidx[k], &g_vk.fgq[k]);

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = VK_API_VERSION_1_3};
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));
    if (g_vk.cqfam != UINT32_MAX) {
        vkGetDeviceQueue(g_vk.dev, g_vk.cqfam, 0, &g_vk.cqueue);
        VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.ctimeline));
        VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.cqfam};
        VK_CHECK(vkCreateCommandPool(g_vk.dev, &cpi, NULL, &g_vk.cpool));
        LOG("[vk] compute queue: family %u\n", g_vk.cqfam);
    }

    void vk_cache_crash_guard(void);
    if (!g_vk.headless) vk_cache_crash_guard();
    // pipeline cache: cache/vk_pipelines.bin (written by vk_pipeline_cache_save; the shader preparation's process has an
    // empty one: it saves pipeline binaries only)
    size_t sz = 0;
    void *data = NULL;
    FILE *fp = g_vk.headless ? NULL : fopen("cache/vk_pipelines.bin", "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        sz = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = malloc(sz);
        if (fread(data, 1, sz, fp) != sz) sz = 0;
        fclose(fp);
    }
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz,
                                    .pInitialData = data};
    extern bool g_pcache_had_data;
    g_pcache_had_data = sz > 0;
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        pc.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    free(data);
}

// The preparation's process on a GPU without pipeline binaries (exec.c vk_prepare_main): the game's pipeline cache file
// merged into its own, the libraries compiled into it and saved for the game, which finds them there at their first
// draw (and the driver's own shader cache has them too - Mesa's on the Steam Deck)
void vk_pcache_load_headless(void) {
    FILE *fp = fopen("cache/vk_pipelines.bin", "rb");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    size_t sz = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    void *data = sz ? malloc(sz) : NULL;
    bool ok = data && fread(data, 1, sz, fp) == sz;
    fclose(fp);
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz, .pInitialData = data};
    VkPipelineCache tmp;
    if (ok && vkCreatePipelineCache(g_vk.dev, &pc, NULL, &tmp) == VK_SUCCESS) {
        vkMergePipelineCaches(g_vk.dev, g_vk.pcache, 1, &tmp);
        vkDestroyPipelineCache(g_vk.dev, tmp, NULL);
    }
    free(data);
}

static pthread_mutex_t pcache_save_lock = PTHREAD_MUTEX_INITIALIZER;  // (the exit and an autosave at once)
void vk_pipeline_cache_save(void) {
    pthread_mutex_lock(&pcache_save_lock);
    size_t sz = 0;
    void *data = NULL;
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) == VK_SUCCESS && sz && (data = malloc(sz)) &&
        vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data) == VK_SUCCESS) {
        mkdir("cache", 0755);
        FILE *f = fopen("cache/vk_pipelines.bin.tmp", "wb");
        if (f) {
            bool ok = fwrite(data, 1, sz, f) == sz;
            ok = fclose(f) == 0 && ok;
            if (!ok || !SDL_RenamePath("cache/vk_pipelines.bin.tmp", "cache/vk_pipelines.bin"))
                remove("cache/vk_pipelines.bin.tmp");
        }
    }
    free(data);
    pthread_mutex_unlock(&pcache_save_lock);
}

// The pipeline cache is also saved while playing, 30 s after new compiles, on a thread of its own: a crash or a killed
// process lost every compile of the run (the next start compiled them all again), and NVIDIA's own shader cache holds
// 1 GB by default, too little to keep a play-through's programs.
_Atomic unsigned g_pcache_dirty;  // (exec.c: libraries and pipelines compiled since the last save - not cache hits)
_Atomic unsigned g_lists_dirty;   // (exec.c: the small lists changed - recipes, drawn programs, first-scene pipelines)
static _Atomic bool autosave_pcache;  // (this autosave writes the pipeline cache too)
void vk_pipeline_recipes_save(void);
void vk_pipeline_prewarm(bool build);
static bool recovering_pipeline_cache;
static _Atomic bool autosave_running;
static _Atomic uint64_t autosave_ns;  // (how long the last one took: a big cache is saved less often)
static void *autosave_thread(void *a) {
    (void)a;
    uint64_t t0 = plat_now_ns();
    bool pcache = atomic_load(&autosave_pcache);
    if (pcache) vk_pipeline_cache_save();  // (the big one: only when the driver compiled something)
    vk_pipeline_recipes_save();
    extern void vk_drawn_programs_save(void);
    vk_drawn_programs_save();
    if (pcache) atomic_store(&autosave_ns, plat_now_ns() - t0);
    static int nlog;
    extern uint64_t renderer_present_count(void);
    if (nlog++ < 50) LOG("[vk] %s saved while playing: %.0f ms (frame %llu)\n", pcache ? "pipeline cache" : "pipeline lists",
                         (plat_now_ns() - t0) / 1e6, (unsigned long long)renderer_present_count());
    atomic_store(&autosave_running, false);
    return NULL;
}
static void pcache_autosave(void) {
    static int on = -1;
    if (on < 0) on = !getenv("ODYSSEY_NO_AUTOSAVE");
    if (!on) return;
    static uint64_t last;
    uint64_t now = plat_now_ns(), every = 30000000000ull;
    if (atomic_load(&autosave_ns) * 20 > every) every = atomic_load(&autosave_ns) * 20;
    if (!last) last = now;
    if (now - last < every || (!atomic_load(&g_pcache_dirty) && !atomic_load(&g_lists_dirty)) ||
        atomic_load(&autosave_running))
        return;
    last = now;
    atomic_store(&autosave_pcache, atomic_exchange(&g_pcache_dirty, 0) != 0);
    atomic_store(&g_lists_dirty, 0);
    atomic_store(&autosave_running, true);
    pthread_t t;
    if (pthread_create(&t, NULL, autosave_thread, NULL) == 0) pthread_detach(t);
    else atomic_store(&autosave_running, false);
}

// Guard Vulkan cache loading and prewarming. Once they return, unrelated game or
// SDL startup failures must not invalidate the prepared pipelines.
// A full shader preparation finished (or the player closed it). Its name carries the translation's generation: raise
// it when the translator's output changes for shaders already prepared (2: TMML's computed level clamped at 0; 3: static
// SSY / PBK / PCNT stacks - with exec.c's PRECOMPILE_LIST, whose programs' SPIR-V keys changed too), so an
// updated install prepares again instead of compiling the changed shaders while playing.
#define PREPARED_MARK "cache/prepared-3"
#define CACHE_START_MARKER "cache/starting"
#define CACHE_START_TAG "vulkan-loading-cache-v3\n"
static _Atomic bool cache_start_pending;
static bool cache_start_interrupted(void) {
    char tag[sizeof CACHE_START_TAG];
    FILE *f = fopen(CACHE_START_MARKER, "r");
    if (!f) return false;
    bool pending = fgets(tag, sizeof tag, f) && !strcmp(tag, CACHE_START_TAG);
    fclose(f);
    return pending;
}

void vk_cache_crash_guard(void) {
    if (cache_start_interrupted()) {
        recovering_pipeline_cache = true;
        LOG("[vk] the last start ended early: the driver's pipeline caches are set aside (once)\n");
        remove("cache/vk_pipelines.bin.old");
        rename("cache/vk_pipelines.bin", "cache/vk_pipelines.bin.old");
        remove("cache/prepare.list");
        remove(PREPARED_MARK);
        remove("cache/precompiled");  // (the marker before generations)
        remove("cache/precompile.list");
        remove("cache/precompile.next");
        remove("cache/precompile-v2.list");
        remove("cache/precompile-v2.next");
        remove("cache/precompile-v3.list");
        remove("cache/precompile-v3.next");
        // Keep the captured render states. The recovery boot loads the recipes without building them.
        DIR *d = opendir("cache/pbin");
        for (struct dirent *e; d && (e = readdir(d)) != NULL;) {
            if (e->d_name[0] == '.') continue;
            char path[600];
            snprintf(path, sizeof path, "cache/pbin/%s", e->d_name);
            remove(path);
        }
        if (d) closedir(d);
    }
    mkdir("cache", 0755);
    FILE *f = fopen(CACHE_START_MARKER, "w");
    if (f) {
        fputs(CACHE_START_TAG, f);
        fclose(f);
        atomic_store(&cache_start_pending, true);
    }
}
static void vk_start_ok(void) {
    if (atomic_exchange(&cache_start_pending, false)) remove(CACHE_START_MARKER);
}

// ================================================================== swapchain
// (Windows: while FSR frame generation is on, the swapchain is AMD's - its acquire / present / images instead of
// Vulkan's: fsr3_sdk.c)
// (and with NVIDIA Streamline, its proxies for the swapchain functions: sl_glue.cpp)
#ifdef _WIN32
#define SL_FN(type, name) static type p; static bool got; \
    if (!got && vk_sl_active()) p = (type)vk_sl_device_proc(g_vk.dev, name); got = got || vk_sl_active();
#endif
static VkResult sc_acquire(VkSemaphore sem, uint32_t *image) {
#ifdef _WIN32
    if (vk_fg_active()) return vk_fg_acquire(g_vk.swapchain, sem, image);
    SL_FN(PFN_vkAcquireNextImageKHR, "vkAcquireNextImageKHR")
    if (p) {
        extern uint64_t renderer_present_count(void);
        vk_sl_frame_start(renderer_present_count());  // (Reflex: its sleep, then this frame's first markers)
    }
    if (p) return p(g_vk.dev, g_vk.swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, image);
#endif
    return vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, image);
}
static VkResult sc_present(VkQueue q, const VkPresentInfoKHR *pi) {
#ifdef _WIN32
    if (vk_fg_active()) return vk_fg_present(q, pi);
    SL_FN(PFN_vkQueuePresentKHR, "vkQueuePresentKHR")
    if (p) {
        extern uint64_t renderer_present_count(void);
        uint64_t frame = renderer_present_count();
        vk_sl_before_present(frame, g_vk.sc_extent.width, g_vk.sc_extent.height);
        VkResult r = p(q, pi);
        vk_sl_marker(frame, 5);  // (present end)
        return r;
    }
#endif
    return vkQueuePresentKHR(q, pi);
}
static VkResult sc_create(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc) {
#ifdef _WIN32
    SL_FN(PFN_vkCreateSwapchainKHR, "vkCreateSwapchainKHR")
    if (p) return p(g_vk.dev, ci, NULL, sc);
#endif
    return vkCreateSwapchainKHR(g_vk.dev, ci, NULL, sc);
}
static void sc_destroy(VkSwapchainKHR sc) {
#ifdef _WIN32
    SL_FN(PFN_vkDestroySwapchainKHR, "vkDestroySwapchainKHR")
    if (p) { p(g_vk.dev, sc, NULL); return; }
#endif
    vkDestroySwapchainKHR(g_vk.dev, sc, NULL);
}
static VkResult sc_images(uint32_t *n, VkImage *images) {
#ifdef _WIN32
    SL_FN(PFN_vkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR")
    if (p) return p(g_vk.dev, g_vk.swapchain, n, images);
#endif
    return vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, n, images);
}
static void device_wait_idle(void) {
#ifdef _WIN32
    SL_FN(PFN_vkDeviceWaitIdle, "vkDeviceWaitIdle")
    if (p) { p(g_vk.dev); return; }
#endif
    vkDeviceWaitIdle(g_vk.dev);
}
static VkPresentModeKHR present_mode(void) {
    // The menu's VSync (Display tab: settings.h VSYNC_*, named as in Savage Wind Recomp): On FIFO, Fast MAILBOX,
    // Adaptive FIFO_RELAXED, Off IMMEDIATE (the default). Fast and Off never make the game wait for the display: it
    // paces itself (vi.c's emulated 60 Hz vsync). With FIFO every submission queues behind the one waiting for a
    // window image, so a throttled / captured / busy compositor slows the whole game (4-21 fps with the GPU mostly
    // idle; mailbox: 60). ODYSSEY_VK_PRESENT=fifo|mailbox|relaxed|immediate overrides it (tests).
    const char *e = getenv("ODYSSEY_VK_PRESENT");
    static const VkPresentModeKHR by_setting[VSYNC_COUNT] = {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR,
                                                             VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR};
    VkPresentModeKHR want = e ? (!strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                                 : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                 : !strcmp(e, "relaxed") ? VK_PRESENT_MODE_FIFO_RELAXED_KHR : VK_PRESENT_MODE_FIFO_KHR)
                              : by_setting[g_settings.vsync >= 0 && g_settings.vsync < VSYNC_COUNT ? g_settings.vsync : VSYNC_OFF];
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    // The closest the driver has: Off -> Fast (neither waits for the display; gamescope / Wayland may lack immediate),
    // otherwise FIFO_RELAXED before FIFO (AMD's Windows driver in exclusive fullscreen, issue #13: slow motion on a ROG
    // Ally only there - a late image goes out at once instead of waiting another refresh); Fast with neither of those
    // (Intel: fifo and immediate only) -> Off, since FIFO would make the game wait for the display. Logged, with what
    // the driver offers.
    static const char *const names[] = {"immediate", "mailbox", "fifo", "fifo relaxed"};
    VkPresentModeKHR order[4] = {want, VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR};
    if (want == VK_PRESENT_MODE_IMMEDIATE_KHR) order[1] = VK_PRESENT_MODE_MAILBOX_KHR, order[2] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    if (want == VK_PRESENT_MODE_MAILBOX_KHR) order[2] = VK_PRESENT_MODE_IMMEDIATE_KHR;
    VkPresentModeKHR got = VK_PRESENT_MODE_FIFO_KHR;
    bool found = false;
    for (int k = 0; k < 4 && !found; k++)
        for (uint32_t i = 0; i < n; i++) if (m[i] == order[k]) { got = order[k]; found = true; break; }
    static VkPresentModeKHR logged = (VkPresentModeKHR)-1;
    if (got != logged) {
        logged = got;
        char offered[64] = "";
        for (uint32_t i = 0; i < n; i++)
            if ((uint32_t)m[i] < 4) snprintf(offered + strlen(offered), sizeof offered - strlen(offered), "%s%s", i ? ", " : "", names[m[i]]);
        LOG("[vk] present mode: %s (the driver offers %s)\n", (uint32_t)got < 4 ? names[got] : "?", offered);
    }
    return got;
}

static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps));
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    if (!ext.width || !ext.height) return;  // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote (see the raw views in the present pass): a UNORM swapchain like Metal's
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    uint32_t count = caps.minImageCount + 1;
    if (getenv("ODYSSEY_VK_IMAGES") && atoi(getenv("ODYSSEY_VK_IMAGES")) > 0) count = (uint32_t)atoi(getenv("ODYSSEY_VK_IMAGES"));  // (tests)
    if (count < caps.minImageCount) count = caps.minImageCount;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainKHR old = g_vk.swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old};
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    g_vk.sc_count = 0;
    bool fg_made = false;
#ifdef _WIN32
    {   // FSR frame generation on / off (the setting, Display tab: a swapchain remake)
        bool want = vk_fg_wanted();
        if (!want && vk_fg_active()) {
            vk_fg_teardown(&g_vk.swapchain);
            old = ci.oldSwapchain = VK_NULL_HANDLE;
        }
        if (want) {
            g_vk.sc_format = sf.format;  // (its context's back buffer format)
            VkSwapchainKHR sc = old;
            if (old && !vk_fg_active()) vk_pacing_retire();
            fg_made = vk_fg_swapchain(&ci, &sc) == VK_SUCCESS;
            if (fg_made) g_vk.swapchain = sc;
            else if (!vk_fg_active()) old = ci.oldSwapchain = g_vk.swapchain = sc;  // (failed: a plain one below)
            else { vk_fg_teardown(&g_vk.swapchain); old = ci.oldSwapchain = VK_NULL_HANDLE; }  // (remade badly: plain)
        }
    }
#endif
    if (!fg_made) {
        VK_CHECK(sc_create(&ci, &g_vk.swapchain));
        if (old) vk_pacing_retire(), sc_destroy(old);
    }
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    g_vk.sc_count = 8;
#ifdef _WIN32
    if (fg_made) {
        uint32_t n = 0;  // (the count first: its get-images fills as many as asked, not as it has)
        vk_fg_get_images(g_vk.swapchain, &n, NULL);
        g_vk.sc_count = n < 8 ? n : 8;
        VkResult gr = vk_fg_get_images(g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
        LOG("[fg] swapchain images: %d, %u (first %p)\n", gr, g_vk.sc_count, (void *)g_vk.sc_images[0]);
    }
    else
#endif
    {   // (the count first, then the images: a proxy - Streamline - fills as many as asked)
        uint32_t n = 0;
        sc_images(&n, NULL);
        g_vk.sc_count = n < 8 ? n : 8;
        sc_images(&g_vk.sc_count, g_vk.sc_images);
    }
#ifdef _WIN32
    vk_sl_swapchain(g_vk.sc_count, sf.format);
#endif
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
}

void vk_queue_lock(void), vk_queue_unlock(void);
static void recreate_swapchain(void) {
#ifdef _WIN32
    vk_fg_wait();  // (frame generation's presenter: its queues quiet too)
#endif
    vk_queue_lock();  // (vkDeviceWaitIdle: no submission on the queue meanwhile - exec.c, the presenter)
    device_wait_idle();
    vk_queue_unlock();
    create_swapchain();
}

// ================================================================== frames
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[8];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

typedef struct { uint32_t image; VkSemaphore acquired; bool ok; } Frame;
static Frame cur_frame;
// One present at a time (the swapchain, its semaphores, cur_frame): vk_frame_begin .. vk_frame_end, and frame
// interpolation's presenter (present_fast), which presents without the render lock. Lock order: render, present, queue.
static pthread_mutex_t g_present_lock;
static void present_lock_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_present_lock, &a);
}
static void present_lock(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, present_lock_init);
    pthread_mutex_lock(&g_present_lock);
}
static void present_unlock(void) { pthread_mutex_unlock(&g_present_lock); }
static bool g_need_recreate;  // (present lock) the presenter saw the swapchain out of date: the next vk_frame_begin remakes it
void vk_request_swapchain_remake(void) { g_need_recreate = true; }  // (the menu: VSync changed)
// Composing a window image ahead (frame interpolation, compose below): vk_present_draw renders into it as into the
// window's (its post passes suspend and resume that rendering: vk_frame_suspend / vk_frame_resume)
static struct { bool on; VkImageView view; VkExtent2D ext; } comp;

// Acquire the next window image and begin rendering into it (cleared to `clear`). Returns false when there is
// nothing to draw into (minimised window).
uint64_t g_pw_wait_ns, g_pw_acquire_ns, g_pw_present_ns;  // (the last present's GPU wait, acquire, present: [hitch])
bool vk_frame_begin(const float clear[4]) {
    present_lock();  // (until vk_frame_end)
    cur_frame.ok = false;
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (!w || !h) { present_unlock(); return false; }  // minimised
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height || g_need_recreate) {
        g_need_recreate = false;
        recreate_swapchain();
    }
    if (!g_vk.swapchain) { present_unlock(); return false; }
    static int trace = -1;
    if (trace < 0) trace = getenv("ODYSSEY_VK_TRACE") != NULL;
    if (trace) LOG("[vk] frame begin %dx%d (swapchain %ux%u)\n", w, h, g_vk.sc_extent.width, g_vk.sc_extent.height);
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    uint64_t pw0 = plat_now_ns();
    vk_wait_seq(slot_seq[slot]);
    uint64_t pw1 = plat_now_ns();
    VkResult r = sc_acquire(acquire_sem[slot], &cur_frame.image);
    g_pw_wait_ns = pw1 - pw0, g_pw_acquire_ns = plat_now_ns() - pw1;
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        r = sc_acquire(acquire_sem[slot], &cur_frame.image);
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {  // (out of date again: the window is being resized / restored)
        static uint64_t nfail;
        if (nfail++ % 600 == 0) LOG("[vk] acquire failed: %d (%llu times)\n", r, (unsigned long long)nfail);
        present_unlock();
        return false;
    }
    cur_frame.acquired = acquire_sem[slot];
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color.float32 = {clear[0], clear[1], clear[2], clear[3]}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    cur_frame.ok = true;
    return true;
}

// Passes of their own between vk_frame_begin and vk_frame_end (exec.c post passes): end the window image's
// rendering, and begin it again keeping what was drawn.
void vk_frame_suspend(void) {
    if (cur_frame.ok || comp.on) vkCmdEndRendering(vk_cmd());
}
void vk_frame_resume(void) {
    if (!cur_frame.ok && !comp.on) return;
    VkCommandBuffer cb = vk_cmd();
    VkExtent2D ext = comp.on ? comp.ext : g_vk.sc_extent;
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = comp.on ? comp.view : g_vk.sc_views[cur_frame.image],
                                    .imageLayout = comp.on ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, ext}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)ext.width, (float)ext.height, 0, 1};
    VkRect2D sc = {{0, 0}, ext};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

VkExtent2D vk_frame_extent(void) { return g_vk.sc_extent; }
void *vk_frame_cmd(void) { return (void *)vk_cmd(); }

static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    *out = vk_readback((uint64_t)w * h * 4);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->buf, 1, &rg);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

// End rendering into the window image and present it. shot_path: also save the window image as PNG.
void vk_frame_end(const char *shot_path) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    vk_checkpoint(cb, "frame end (window image %u)", cur_frame.image);
    vkCmdEndRendering(cb);
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    uint32_t img = cur_frame.image;
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done_sem[img],
                           .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
    uint64_t pp0 = plat_now_ns();
    VkPresentIdKHR pid;
    uint64_t pidv = 0;
    vk_pacing_tag(&pi, &pid, &pidv);
    vk_queue_lock();
    bool sent = !atomic_load(&g_no_present);
    VkResult r = !sent ? VK_SUCCESS : sc_present(g_vk.queue, &pi);
    vk_queue_unlock();
    vk_pacing_presented(pidv, sent && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR));
    g_pw_present_ns = plat_now_ns() - pp0;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreate_swapchain();
    cur_frame.ok = false;
    if (shot_path) {
        vk_wait_seq(seq);
        uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
        uint8_t *px = shot.ptr;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint64_t i = 0; bgr && i < (uint64_t)w * h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
        vk_save_png(shot_path, px, w, h);
    }
    present_unlock();  // (vk_frame_begin's)
}

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
    // ODYSSEY_SHOT_FAST=1 (tests): uncompressed BMP data under the same name - a PNG took ~165 ms and a series of shots
    // ran the game at 3 fps, where it behaves differently (no compute dispatches); image readers sniff the format
    static int fast = -1;
    if (fast < 0) fast = getenv("ODYSSEY_SHOT_FAST") != NULL;
    if (!s || !(fast ? SDL_SaveBMP(s, path) : SDL_SavePNG(s, path))) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    SDL_DestroySurface(s);
}

// ================================================================== game presentation (metal.m backend_present)
// Draws the game's image into the window with the widescreen crop, then the overlay menu.
extern void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]);
void renderer_submit(void);
void renderer_set_scale(float sx, float sy);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);

static uint64_t frames, held_frames, total_frames, g_frame_counter;
uint64_t renderer_present_count(void) { return total_frames; }
uint64_t vk_frame_counter(void) { return g_frame_counter; }
static float g_shot_crop[4] = {0, 0, 1, 1};
static double last_fps_time;
static NvnTexture *g_last_presented;

const float *vk_shot_crop(void) { return g_shot_crop; }

void backend_present(NvnTexture *t);
void backend_represent(void) {
    extern void rq_drain(void);
    rq_drain();  // (after what the render thread has queued: runtime/nvn/rqueue.c)
    if (g_last_presented) backend_present(g_last_presented);
    else SDL_Delay(16);
}

// ---- frame interpolation (runtime/nvn/interp.c): images between the game's frames
// One of our images made like a texture's (2D, one level), for a copy of it.
static void hold_make(const TexBackend *src, TexBackend *dst) {
    if (dst->image && dst->width == src->width && dst->height == src->height && dst->format == src->format) return;
    if (dst->image) { vk_views_forget(dst); vk_defer_image(dst->image, dst->mem); }
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                            .imageType = VK_IMAGE_TYPE_2D, .format = src->format, .extent = {src->width, src->height, 1},
                            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    *dst = (TexBackend){.format = src->format, .base_type = VK_IMAGE_VIEW_TYPE_2D, .width = src->width, .height = src->height,
                        .depth = 1, .layers = 1, .levels = 1, .samples = 1, .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .scalable = src->scalable,
                        .sx = src->sx, .sy = src->sy, .gpu_owned = true, .uploaded = true};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &dst->image, &dst->mem, NULL));
    vk_image_init_layout(vk_cmd(), dst->image, VK_IMAGE_ASPECT_COLOR_BIT);
}
static void image_copy(const TexBackend *src, const TexBackend *dst) {
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .extent = {src->width, src->height, 1}};
    vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    vk_full_barrier(cb);
}

// The display's refresh rate (0: unknown): in-betweens only pay off at twice the game's 60 or more - at 60 Hz they
// would replace each other before the display shows them.
static float display_hz(void) {
    SDL_DisplayID d = window ? SDL_GetDisplayForWindow(window) : 0;
    const SDL_DisplayMode *m = d ? SDL_GetCurrentDisplayMode(d) : NULL;
    const SDL_DisplayMode *desktop = d ? SDL_GetDesktopDisplayMode(d) : NULL;
    // Windows can report the instantaneous VRR rate here. Borderless uses the
    // desktop mode's nominal rate; exclusive fullscreen uses its selected mode.
    const SDL_DisplayMode *fullscreen = window ? SDL_GetWindowFullscreenMode(window) : NULL;
    float hz = m ? m->refresh_rate : 0;
    if (fullscreen && fullscreen->refresh_rate > 0) return fullscreen->refresh_rate;
    if (desktop && desktop->refresh_rate > hz) hz = desktop->refresh_rate;
    return hz;
}

// The presenter: shows queued images at their times, in order. They are window images composed ahead (compose: the
// game's image placed, post-processed and letterboxed at the window's size, as a present draws it), which the presenter
// copies into the swapchain's and presents with command buffers of its own (present_fast) - without the render lock,
// which the game holds ~4.5 ms a frame for its command list and a replay ~3.5 ms: waiting for it, presents came up to
// 6 ms late (17% of them over 1 ms), unevenly spaced. With the menu open (it takes input and runs its actions under the
// render lock) or the swapchain to remake, a present goes the usual way (present_full). An image older than the latest
// present is dropped (never a step back: the game thread presents itself again when interpolation stops or skips a
// frame). Order: 2 x the game's frame for its own image, one less for the in-between before it.
// ODYSSEY_LOG_PRESENTS=1: each present, how late it was and how long it waited for the locks (ms; "slow": the usual way).
typedef struct { TexBackend *img; uint64_t at, order; const char *what; } PJob;
static PJob pjobs[8];
static int npjobs;
static pthread_mutex_t pres_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pres_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_present_order;  // (present lock) the latest present's order

// (render lock) one of our window images: the swapchain's format and size, in the general layout like the game's
static bool winimg_make(TexBackend *w) {
    VkExtent2D e = g_vk.sc_extent;
    if (!g_vk.swapchain || !e.width || !e.height) return false;
    if (w->image && w->width == e.width && w->height == e.height && w->format == g_vk.sc_format) return true;
    if (w->image) { vk_views_forget(w); vk_defer_image(w->image, w->mem); }
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = g_vk.sc_format,
                            .extent = {e.width, e.height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    *w = (TexBackend){.format = g_vk.sc_format, .base_type = VK_IMAGE_VIEW_TYPE_2D, .width = e.width, .height = e.height,
                      .depth = 1, .layers = 1, .levels = 1, .samples = 1, .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .sx = 1, .sy = 1,
                      .gpu_owned = true, .uploaded = true};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &w->image, &w->mem, NULL));
    vk_image_init_layout(vk_cmd(), w->image, VK_IMAGE_ASPECT_COLOR_BIT);
    return true;
}

// (render lock) src as a present draws it into the window (placement v, parameters pv: vk_present_draw), into the window
// image w - what the presenter copies into the swapchain's
static bool compose(TexBackend *w, TexBackend *src, const float v[4], const float pv[8]) {
    if (!src || !src->image || !winimg_make(w)) return false;
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageView view = vk_image_view(w, VK_IMAGE_VIEW_TYPE_2D, w->format, 0, 1, 0, 1, (VkComponentMapping){0}, VK_IMAGE_ASPECT_COLOR_BIT);
    VkExtent2D ext = {w->width, w->height};
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.color.float32 = {0, 0, 0, 1}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, ext}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)ext.width, (float)ext.height, 0, 1};
    VkRect2D sc = {{0, 0}, ext};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    comp.on = true, comp.view = view, comp.ext = ext;
    vk_present_draw(cb, src, v[0], v[1], v[2], v[3], pv);
    comp.on = false;
    cb = vk_cmd();
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    return true;
}

// (render lock) the usual way: a window image over the whole window, the overlay (the menu) over it
static bool present_full(TexBackend *w) {
    static const float black[4] = {0, 0, 0, 1}, pv[8] = {0, 0, 0, 0, 0, 0, 1, 1};  // (composed: no flip, no crop)
    vk_end_pass();
    if (!vk_frame_begin(black)) return false;
    VkCommandBuffer cb = vk_cmd();
    vk_present_draw(cb, w, 0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, pv);
    vk_queue_lock();  // (ImGui's first frame uploads its fonts through the queue)
    menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
    vk_queue_unlock();
    vk_frame_end(NULL);
    return true;
}

// (present lock) the presenter's own submission: the window image copied into the swapchain's, the FPS counter over it
// (menu_overlay_lockfree: nothing else open), presented. false: the usual way instead.
#define FAST_N 4
static VkCommandPool fast_pool;
static VkCommandBuffer fast_cb[FAST_N];
static VkFence fast_fence[FAST_N];
static VkSemaphore fast_acq[FAST_N];
static int fast_i;
static bool present_fast(TexBackend *w) {
    if (!menu_overlay_lockfree() || g_need_recreate || !g_vk.swapchain) return false;
    int ww, wh;
    SDL_GetWindowSizeInPixels(window, &ww, &wh);
    VkExtent2D e = g_vk.sc_extent;
    if (!ww || !wh || (uint32_t)ww != e.width || (uint32_t)wh != e.height) return false;
    if (!fast_pool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = g_vk.qfam};
        VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &fast_pool));
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = fast_pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = FAST_N};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, fast_cb));
        for (int i = 0; i < FAST_N; i++) {
            VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
            VK_CHECK(vkCreateFence(g_vk.dev, &fi, NULL, &fast_fence[i]));
            fast_acq[i] = binary_semaphore();
        }
    }
    int k = fast_i = (fast_i + 1) % FAST_N;
    vkWaitForFences(g_vk.dev, 1, &fast_fence[k], VK_TRUE, UINT64_MAX);  // (its semaphore and command buffer free again)
    uint32_t img;
    VkResult r = sc_acquire(fast_acq[k], &img);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { g_need_recreate = true; return false; }
    if (r == VK_SUBOPTIMAL_KHR) g_need_recreate = true;  // (this one still goes out)
    vkResetFences(g_vk.dev, 1, &fast_fence[k]);
    VkCommandBuffer cb = fast_cb[k];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));
    vk_full_barrier(cb);  // (after the compose, submitted before)
    VkImage si_ = g_vk.sc_images[img];
    image_barrier(cb, si_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (w->width == e.width && w->height == e.height) {
        VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .extent = {e.width, e.height, 1}};
        vkCmdCopyImage(cb, w->image, VK_IMAGE_LAYOUT_GENERAL, si_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    } else {  // (composed before the window changed size)
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)w->width, (int32_t)w->height, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)e.width, (int32_t)e.height, 1}}};
        vkCmdBlitImage(cb, w->image, VK_IMAGE_LAYOUT_GENERAL, si_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    }
    bool stats = menu_stats_visible();
    if (stats) {
        image_barrier(cb, si_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[img],
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, e}, .layerCount = 1,
                              .colorAttachmentCount = 1, .pColorAttachments = &ca};
        vkCmdBeginRendering(cb, &ri);
        VkViewport vp = {0, 0, (float)e.width, (float)e.height, 0, 1};
        VkRect2D sc = {{0, 0}, e};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vk_queue_lock();
        menu_render((void *)cb, e.width, e.height);
        vk_queue_unlock();
        vkCmdEndRendering(cb);
    }
    VkImageLayout lay = stats ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    // ODYSSEY_WINDOW_SHOT=<frame>[:<n>] (tests): what the first n presents from that frame on put into the window ->
    // build/window.png (n > 1: build/window_<i>.png), as a direct present does (backend_present_locked)
    static long ws_at = -1, ws_n;
    static int ws_i;
    if (ws_at < 0) {
        const char *ev = getenv("ODYSSEY_WINDOW_SHOT");
        ws_at = ev ? atol(ev) : 0;
        ws_n = ev && strchr(ev, ':') ? atol(strchr(ev, ':') + 1) : 1;
    }
    VkBuffer sbuf = VK_NULL_HANDLE;
    VmaAllocation smem = NULL;
    VmaAllocationInfo sinfo = {0};
    // (a test script's "frame:shot:<name>" - runtime/hle/input.c - asks for this present too)
    extern char g_window_shot_request[256];
    char req[256] = "";
    if (g_window_shot_request[0]) snprintf(req, sizeof req, "%s", g_window_shot_request), g_window_shot_request[0] = 0;
    if (req[0] || (ws_at && (long)total_frames + 1 >= ws_at && ws_i < ws_n)) {
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)e.width * e.height * 4,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                       .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        if (vmaCreateBuffer(g_vk.vma, &bci, &aci, &sbuf, &smem, &sinfo) == VK_SUCCESS) {
            image_barrier(cb, si_, lay, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy rg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {e.width, e.height, 1}};
            vkCmdCopyImageToBuffer(cb, si_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sbuf, 1, &rg);
            lay = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        } else sbuf = VK_NULL_HANDLE;
    }
    image_barrier(cb, si_, lay, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VK_CHECK(vkEndCommandBuffer(cb));
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSemaphoreSubmitInfo wt = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = fast_acq[k],
                                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSemaphoreSubmitInfo sg = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = done_sem[img],
                                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &wt,
                        .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi, .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &sg};
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done_sem[img],
                           .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
    VkPresentIdKHR pid;
    uint64_t pidv = 0;
    vk_pacing_tag(&pi, &pid, &pidv);
    vk_queue_lock();
    VkResult sr = vkQueueSubmit2(g_vk.queue, 1, &si, fast_fence[k]);
    bool sent = sr == VK_SUCCESS && !atomic_load(&g_no_present);
    VkResult pr = sr != VK_SUCCESS ? sr : !sent ? VK_SUCCESS : sc_present(g_vk.queue, &pi);
    vk_queue_unlock();
    vk_pacing_presented(pidv, sent && (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR));
    VK_CHECK(sr);
    static int log_gpu = -1;  // ODYSSEY_LOG_PRESENT_GPU=1: when its copy finished on the GPU, in the [display] log
    if (log_gpu < 0) log_gpu = getenv("ODYSSEY_LOG_PRESENT_GPU") != NULL;
    if (log_gpu && pidv) {
        vkWaitForFences(g_vk.dev, 1, &fast_fence[k], VK_TRUE, UINT64_MAX);
        vk_pacing_gpu_done(pidv, plat_now_ns());
    }
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) g_need_recreate = true;
    if (sbuf) {
        vkWaitForFences(g_vk.dev, 1, &fast_fence[k], VK_TRUE, UINT64_MAX);
        uint8_t *px = sinfo.pMappedData;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint64_t i = 0; i < (uint64_t)e.width * e.height; i++) {
            if (bgr) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
            px[i * 4 + 3] = 255;
        }
        char path[1024];
        if (req[0]) snprintf(path, sizeof path, "%s", req);
        else if (ws_n > 1) snprintf(path, sizeof path, "build/window_%02d.png", ws_i);
        else snprintf(path, sizeof path, "%s", getenv("ODYSSEY_WINDOW_SHOT_PATH") ? getenv("ODYSSEY_WINDOW_SHOT_PATH") : "build/window.png");
        vk_save_png(path, px, e.width, e.height);
        vmaDestroyBuffer(g_vk.vma, sbuf, smem);
        if (req[0]) return true;
        if (++ws_i == ws_n && getenv("ODYSSEY_EXIT_AFTER_SHOT")) { extern void sampler_flush(void); sampler_flush(); SL_TEST_EXIT(); fflush(stderr); _exit(0); }
    }
    return true;
}

extern __thread const char *g_lock_role;  // (exec.c: ODYSSEY_LOG_LOCKS)
static void *presenter_thread(void *arg) {
    (void)arg;
    g_lock_role = "presenter";
    plat_thread_realtime(0);  // (on time: the images' spacing is the point)
    for (;;) {
        pthread_mutex_lock(&pres_mx);
        while (!npjobs) pthread_cond_wait(&pres_cv, &pres_mx);
        PJob j = pjobs[0];
        memmove(pjobs, pjobs + 1, (size_t)--npjobs * sizeof pjobs[0]);
        pthread_mutex_unlock(&pres_mx);
        plat_sleep_until_ns(j.at);
        uint64_t t0 = plat_now_ns();
        present_lock();
        uint64_t t1 = plat_now_ns();
        bool stale = j.order <= g_present_order, fast = !stale && present_fast(j.img), slow = false;
        if (fast) g_present_order = j.order;
        present_unlock();
        if (!stale && !fast) {
            vk_render_lock();
            t1 = plat_now_ns();
            present_lock();
            if (j.order > g_present_order && present_full(j.img)) g_present_order = j.order, slow = true;
            present_unlock();
            vk_render_unlock();
        }
        if ((fast || slow) && getenv("ODYSSEY_LOG_PRESENTS")) {
            uint64_t t2 = plat_now_ns();
            LOG("[present] %.3f %s late %.2f lock %.2f%s\n", t2 / 1e6, j.what, ((int64_t)t2 - (int64_t)j.at) / 1e6, (t1 - t0) / 1e6,
                slow ? " slow" : "");
        }
    }
    return NULL;
}
static void presenter_queue(TexBackend *img, uint64_t at, uint64_t order, const char *what) {
    static bool started;
    if (!started) {
        started = true;
        pthread_t th;
        if (pthread_create(&th, NULL, presenter_thread, NULL) == 0) pthread_detach(th);
    }
    pthread_mutex_lock(&pres_mx);
    if (npjobs < 8) pjobs[npjobs++] = (PJob){.img = img, .at = at, .order = order, .what = what};
    pthread_cond_signal(&pres_cv);
    pthread_mutex_unlock(&pres_mx);
}

// The interpolation thread (runtime/nvn/interp.c): a frame's matching, its replay and the presents of its images run
// here. The game hands a frame over as soon as it is complete - its present call, before the wait for the vsync that
// paces it (backend_frame_done) - with its own image composed for the window; this thread matches it against the
// frames before (no locks), replays it with blended uniforms into the game's targets (the render lock; lists the game
// submits meanwhile wait for it - interp_wait_replay - so two frames' draws never interleave), composes the in-between
// too (the game's next frame may draw into the presented texture before the presenter shows it) and queues both: the
// in-between at a delay from the time the game's present is due - the recent replays' latest finish + 1.5 ms, at
// least half a game frame early (the game's own image never shows before its time) - and the game's image half a game
// frame after it: evenly spaced whatever a replay took. In busy views the game thread works ~5 ms of its 16.7 and
// waits for the vsync the rest; the matching and the replay (~7 ms with ~1700 draws) fit in that wait. Started after
// the present instead, they held up the game's next frame (~5 ms a frame): it missed vsyncs and the brake turned
// in-betweens off nearly everywhere.
typedef struct {
    InterpFrame *f;
    NvnTexture *t;          // the presented texture: the replay draws the in-between into it
    TexBackend *game;       // the game's own image (a window image, composed); NULL: the game thread presents it (matching only)
    float v[4], pv[8];
    uint64_t at, frame;     // when the game's present is due, and its number
    double period;          // the game's frame period (ns)
    uint32_t epoch;         // (textures.c g_tex_epoch) the frame's: its images' frame-start copies
} IJob;
static IJob ijobs[2];
static int nijobs, replay_pending;  // (ij_mx) handed-over frames whose replay hasn't run: the game's next lists wait
static pthread_mutex_t ij_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ij_cv = PTHREAD_COND_INITIALIZER, ij_done_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_interp_waited_ns;  // (stats) the game's waits for a replay
static _Atomic int replay_waiters;  // game threads waiting for one now: the replay not started yet is left out

void interp_wait_replay(void) {
    if (!__atomic_load_n(&replay_pending, __ATOMIC_ACQUIRE)) return;
    uint64_t t0 = plat_now_ns();
    atomic_fetch_add(&replay_waiters, 1);
    pthread_mutex_lock(&ij_mx);
    while (replay_pending) pthread_cond_wait(&ij_done_cv, &ij_mx);
    pthread_mutex_unlock(&ij_mx);
    atomic_fetch_sub(&replay_waiters, 1);
    atomic_fetch_add(&g_interp_waited_ns, plat_now_ns() - t0);
}
bool interp_replay_busy(void) { return __atomic_load_n(&replay_pending, __ATOMIC_ACQUIRE) > 0; }  // (rqueue.c)
static void replay_done(void) {
    pthread_mutex_lock(&ij_mx);
    if (replay_pending) __atomic_store_n(&replay_pending, replay_pending - 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&ij_done_cv);
    pthread_mutex_unlock(&ij_mx);
}

static void *interp_thread(void *arg) {
    (void)arg;
    extern void renderer_interp_replay(void);
    g_lock_role = "interp";
    double done[64], delay = 0;  // when recent replays were done, from their frame's present (ns; < 0: before it)
    int nd = 0;
    uint64_t last_log = 0, njobs = 0, nskipped = 0;
    double done_sum = 0;
    // ODYSSEY_INTERP_SHOT=<frame>[:<n>] (tests, videos): the in-betweens before frames frame .. frame + n - 1 ->
    // build/frames/frame_<n>_mid.png (numbered as ODYSSEY_SHOT_AT numbers the frame it precedes); none is left out then
    const char *se = getenv("ODYSSEY_INTERP_SHOT");
    long shot = se ? atol(se) : 0, shot_n = se && strchr(se, ':') ? atol(strchr(se, ':') + 1) : 1;
    for (;;) {
        pthread_mutex_lock(&ij_mx);
        while (!nijobs) pthread_cond_wait(&ij_cv, &ij_mx);
        IJob j = ijobs[0];
        memmove(ijobs, ijobs + 1, (size_t)--nijobs * sizeof ijobs[0]);
        pthread_mutex_unlock(&ij_mx);
        bool blend = interp_frame_begin(j.f);
        if (j.game) {
            TexBackend *mid = NULL;
            // (the game's present is past and it already waits to draw its next frame - a busy scene, a cutscene: this
            // in-between is left out rather than cost the game its vsync; before the present only the game's other
            // threads can wait - a list of theirs, off the frame's path)
            bool waited = blend && !shot && atomic_load(&replay_waiters) > 0 && plat_now_ns() > j.at;
            nskipped += waited;
            if (blend && !waited && interp_replays_allowed(j.frame, total_frames) && interp_prepare(0.5f) && interp_replay_lock()) {
                extern uint32_t g_interp_replay_epoch;
                g_interp_replay_epoch = j.epoch;
                // ODYSSEY_INTERP_DEBUG=<n> (research): the replay making the in-between before frame n runs as a debug
                // frame - its draws logged ([draw]), with ODYSSEY_PROBE the texel's changes ([probe])
                static long dbg_replay = -2;
                if (dbg_replay == -2) { const char *e = getenv("ODYSSEY_INTERP_DEBUG"); dbg_replay = e ? atol(e) : -1; }
                extern bool g_debug_frame;
                bool dbg = dbg_replay >= 0 && (long)j.frame + 1 == dbg_replay;
                bool dbg_was = g_debug_frame;  // (the game's next frame may be one: ODYSSEY_DEBUG_FRAME)
                if (dbg) g_debug_frame = true, LOG("===== replay debug (in-between before %ld) =====\n", dbg_replay);
                renderer_interp_replay();
                if (dbg) g_debug_frame = dbg_was, LOG("===== replay debug end =====\n");
                interp_replay_unlock();
                vk_render_lock();
                extern TexBackend *vk_tex_replayed(NvnTexture *t);
                TexBackend *b = vk_tex_replayed(j.t);  // (textures.c: the replay drew into a twin of the window image)
                static TexBackend mids[3];
                static int mi;
                mid = &mids[mi = (mi + 1) % 3];
                if (shot && (long)j.frame + 1 >= shot && (long)j.frame + 1 < shot + shot_n) {
                    char path[256];
                    mkdir("build/frames", 0755);
                    snprintf(path, sizeof path, "build/frames/frame_%05llu_mid.png", (unsigned long long)j.frame + 1);
                    renderer_save_texture_tex(b, path);
                }
                if (!compose(mid, b, j.v, j.pv)) mid = NULL;
                vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);  // (the GPU starts on it)
                vk_render_unlock();
            } else {
                renderer_submit();  // (the game image's compose: on the GPU before the presenter's copy of it)
            }
            replay_done();
            double fin = (double)((int64_t)plat_now_ns() - (int64_t)j.at);
            done[nd++ % 64] = fin;
            done_sum += fin, njobs++;
            double w = -1e18;
            for (int i = 0; i < 64 && i < nd; i++) if (done[i] > w) w = done[i];
            // (up at once when a replay finished later; down slowly - a step back in the images' timeline is a stutter)
            double want = w + 1.5e6;
            if (want < -j.period / 2) want = -j.period / 2;
            if (want > 14e6) want = 14e6;
            delay = want > delay ? want : delay - fmin(delay - want, 0.05e6);
            int64_t due = (int64_t)j.at + (int64_t)delay;
            uint64_t order = 2 * (j.frame + 1);
            if (mid) presenter_queue(mid, (uint64_t)due, order - 1, "in-between");
            presenter_queue(j.game, (uint64_t)(due + (int64_t)(j.period / 2)), order, "game image");
        }
        interp_frame_rotate();
        interp_log_stats();
        uint64_t now = plat_now_ns();
        if (getenv("ODYSSEY_INTERP_LOG") && now - last_log > 2000000000ull && njobs) {
            LOG("[interp-thread] replays done %.1f ms after (- before) the game's present was due; in-betweens shown %.1f ms after it; "
                "the game waited %.2f ms a frame for replays; %llu left out for it\n", done_sum / njobs / 1e6, delay / 1e6,
                atomic_exchange(&g_interp_waited_ns, 0) / 1e6 / (double)njobs, (unsigned long long)nskipped);
            last_log = now, njobs = 0, done_sum = 0, nskipped = 0;
        }
    }
    return NULL;
}
// (game thread) a closed frame to the interpolation thread; false: its queue is full (the frame is the caller's to drop)
static bool interp_hand_over(const IJob *job) {
    static bool started;
    if (!started) {
        started = true;
        pthread_t th;
        if (pthread_create(&th, NULL, interp_thread, NULL) == 0) pthread_detach(th);
    }
    pthread_mutex_lock(&ij_mx);
    bool ok = nijobs < 2;
    if (ok) {
        ijobs[nijobs++] = *job;
        if (job->game) __atomic_store_n(&replay_pending, replay_pending + 1, __ATOMIC_RELEASE);
        pthread_cond_signal(&ij_cv);
    }
    pthread_mutex_unlock(&ij_mx);
    return ok;
}

// The game's frame: what is shown and how, decided once - when the game hands it over complete (backend_frame_done,
// before its vsync wait) or else at its present.
static struct {
    bool done;             // for frame `frame`
    uint64_t frame;
    bool skip, placed;     // not shown (a full-screen pass still building) / the window has a size
    float v[4], pv[8];     // where in the window (runtime/nvn/output_scale.c), the present pass's parameters
    TexBackend *b;         // the presented texture's image
    TexBackend *game_w;    // with in-betweens: the game's image composed for the window - the interpolation thread presents it
    TexBackend *game_img;  // (screenshots: ODYSSEY_SHOT_AT, ODYSSEY_SCREENSHOT_EVERY) a copy of the game's image - b gets the in-between
} fd;
static double game_period = 16.67e6;  // the game's frame period (present to present, smoothed): the images' spacing
static uint64_t brake_until;          // the brake (present_brake): no in-betweens before this frame

// Render scale + placement in the window: runtime/nvn/output_scale.c (Video tab "Internal resolution"), from the window
// image's size (vk_frame_begin remakes it when the window changed); no GPU timer here (-1): dynamic resolution goes by
// the frame time. The image is 16:9 (bars in other window shapes), or fills a wider window when "Aspect ratio" says so:
// the game widens its view by the factor in effect (runtime/hle/odyssey_wide.c; it takes the window's between game
// frames). ODYSSEY_WIDE_TEST=<f>[@<frame>]: that factor whatever the window (from that frame on: a change while playing).
static bool frame_placement(float v[4], float pv[8]) {
    {   // (Frame rate: Match display - settings.c) the display's refresh, now and then (the window may move)
        static unsigned n;
        if (!(n++ % 60)) g_display_hz = (int)(display_hz() + 0.5f);
    }
    double dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
    memset(v, 0, 4 * sizeof *v);
    memset(pv, 0, 8 * sizeof *pv);
    if (dw <= 0 || dh <= 0) return false;
    static double wide_test = -1;
    static long wide_from;
    if (wide_test < 0) {
        const char *e = getenv("ODYSSEY_WIDE_TEST");
        wide_test = e ? atof(e) : 0;
        wide_from = e && strchr(e, '@') ? atol(strchr(e, '@') + 1) : 0;
    }
    double win = (dw / dh) / (16.0 / 9.0);
    extern void odyssey_wide_want(float k);
    extern float odyssey_wide_shape(void);
    if (wide_test > 0) odyssey_wide_want((long)total_frames >= wide_from ? (float)wide_test : 1.0f);
    else {
        int a = g_settings.aspect >= 0 && g_settings.aspect < ASPECT_COUNT ? g_settings.aspect : ASPECT_16_9;
        double k = a == ASPECT_FILL ? (fabs(win - 1.0) > 0.005 ? win : 1.0) : ASPECT_K[a];
        odyssey_wide_want((float)k);
    }
    OutScale os;
    output_scale(dw, dh, odyssey_wide_shape(), SDL_GetTicksNS() / 1e9, -1, &os);
    {   // (the mouse pointer in the game's menus: where the picture is - runtime/hle/input.c)
        extern void input_set_picture(double vx, double vy, double vw, double vh, double dw, double dh);
        input_set_picture(os.vx, os.vy, os.vw, os.vh, dw, dh);
    }
#ifndef __APPLE__  // (the temporal upscalers: every Vulkan build)
    // Only the automatic choices (Match display, 100%) let an upscaler's mode pick the input resolution - any picture
    // shape (the Steam Deck's 16:10): the targets' scale from the picture's size in the window. Not in handheld mode:
    // its scene is the 1280x720 corner of the 1600x900 window texture, which the upscalers don't take (yet) - a lower
    // resolution there would only be blurrier.
    double dlss_scale = vk_temporal_scale();
    if ((g_settings.res_mode == RES_MATCH || g_settings.res_mode == RES_DISPLAY) && dlss_scale && os.vh >= 480 &&
        !settings_handheld()) {
        // ...rounded to a size the game's own smaller targets divide into whole pixels: width a multiple of 16, height
        // of 60 (a fifteenth of the game's 900 lines). FSR 2 / DLSS Quality at 16:10 (1280 x 800: 853 x 533) left
        // its quarter-size targets at 133.3 lines: the game's lighting passes sampled them off - dark smears over
        // the ground, half-black buildings (owner's Steam Deck report, 2026-11-16). Whole quarters (532 lines) fixed
        // that but not the ground mist (a white dome over Cap Kingdom's snow at 532 and 536 lines, right at 540 -
        // as at 16:9's 480 and 75%'s 600). ODYSSEY_TEMPORAL_SNAP_H=<lines> for tests.
        static double snap_h = 0;
        if (!snap_h) snap_h = getenv("ODYSSEY_TEMPORAL_SNAP_H") ? atof(getenv("ODYSSEY_TEMPORAL_SNAP_H")) : 60;
        double rw = fmax(16, floor(os.vw * dlss_scale / 16 + 0.5) * 16), rh = fmax(snap_h, floor(os.vh * dlss_scale / snap_h + 0.5) * snap_h);
        os.sx = rw / GAME_W;
        os.sy = rh / GAME_H;
    }
#endif
    // (uv origin and size: the game's window crop - in handheld mode the 1280x720 corner of its window texture)
    extern void renderer_present_crop(float crop[4]);
    float crop[4];
    renderer_present_crop(crop);
    renderer_set_scale((float)os.sx, (float)os.sy);
    vk_overlay_resolution((unsigned)(GAME_W * os.sx + 0.5), (unsigned)(GAME_H * os.sy + 0.5), (unsigned)(os.vw + 0.5),
                          (unsigned)(os.vh + 0.5));  // (the performance overlay's "1600 x 900 -> 3840 x 2160")
    v[0] = (float)os.vx, v[1] = (float)os.vy, v[2] = (float)os.vw, v[3] = (float)os.vh;
    pv[0] = renderer_origin_lower_left() ? 1.0f : 0.0f;
    memcpy(pv + 4, crop, sizeof crop);
    memcpy(g_shot_crop, crop, sizeof crop);
    return true;
}

// (render lock) Frame interpolation (runtime/nvn/interp.c): the frame logged while its lists ran is closed and handed to
// the interpolation thread (above) - with its image when it makes in-betweens; ODYSSEY_INTERP=measure only matches it
// (stats). present_at: when its present is due (host ns).
static void frame_decide(NvnTexture *t, uint64_t present_at) {
    TexBackend *b = vk_tex(t, true);
    vk_end_pass();
    g_interp_present_frame = total_frames;
    int interp_want = odyssey_native_120_enabled() ? INTERP_OFF : interp_wanted();
#ifndef __APPLE__  // (the temporal upscalers: every Vulkan build)
    if (vk_temporal_enabled()) interp_want = INTERP_OFF;
#endif
    if (interp_want == INTERP_ON && total_frames < brake_until) interp_want = INTERP_OFF;
    // (in-betweens need a display at 90 Hz or more - ODYSSEY_INTERP=on: any; below, not even the logging)
    bool hz_ok = display_hz() >= 90 || getenv("ODYSSEY_INTERP");
    if (interp_want == INTERP_ON && !hz_ok) {
        static bool said;
        if (!said) { said = true; LOG("[interp] the display runs at %.0f Hz: no in-betweens (they need 90 Hz or more)\n", display_hz()); }
        interp_want = INTERP_OFF;
    }
    InterpFrame *closed = interp_frame_close(interp_want != INTERP_OFF, total_frames);
    extern bool renderer_take_frame_incomplete(void);
    fd.skip = renderer_take_frame_incomplete();  // (a full-screen pass still building: the window keeps the last frame)
    if (fd.skip && getenv("ODYSSEY_LOG_SHADER_TIME"))
        LOG("[shtime] frame %llu not shown (a full-screen pass is building)\n", (unsigned long long)total_frames);
    fd.placed = frame_placement(fd.v, fd.pv);
    fd.b = b;
    fd.game_w = fd.game_img = NULL;
    // With in-betweens the game's image is composed for the window now (the replay draws the in-between into b) and the
    // interpolation thread presents both; three of each, as the presenter may still show an earlier one.
    if (closed && interp_want == INTERP_ON && !fd.skip && fd.placed) {
        static TexBackend wins[3], hold[3];
        static int wi;
        TexBackend *w = &wins[wi = (wi + 1) % 3];
        if (compose(w, b, fd.v, fd.pv)) {
            if (getenv("ODYSSEY_SHOT_AT") || getenv("ODYSSEY_SCREENSHOT_EVERY")) {
                hold_make(b, &hold[wi]);
                image_copy(b, &hold[wi]);
                fd.game_img = &hold[wi];
            }
            extern uint32_t g_tex_epoch;
            IJob job = {.f = closed, .t = t, .game = w, .at = present_at, .frame = total_frames, .period = game_period, .epoch = g_tex_epoch};
            memcpy(job.v, fd.v, sizeof job.v);
            memcpy(job.pv, fd.pv, sizeof job.pv);
            if (interp_hand_over(&job)) fd.game_w = w, closed = NULL;
            else fd.game_img = NULL;
        }
    }
    if (closed) {  // (matching only - measure mode, a frame not shown - or the interpolation thread's queue was full)
        IJob job = {.f = closed, .frame = total_frames};
        if (!interp_hand_over(&job)) interp_frame_drop(closed);
    }
    fd.done = true, fd.frame = total_frames;
    extern uint32_t g_tex_epoch;
    g_tex_epoch++;  // (the next frame's lists: textures.c tracks their reads and first writes under the next number)
}

// (game thread) its frame is complete: the in-between starts while it waits for its present's vsync
void backend_frame_done(NvnTexture *t, uint64_t present_at) {
    vk_render_lock();
    frame_decide(t, present_at);
    vk_render_unlock();
}

// The game's frame period, and the brake: when the game misses its vsync too often with in-betweens - 12 of its last 120
// frames over 18 ms (a hitch counts once) - they pause (the game's own frames at full speed instead) for 2 s, longer
// each time in a row (up to 20 s), and come back; 30 s without: short pauses again. (A smoothed period over 17.4 ms was
// the rule: one late frame pushed it over - the game catching up right after it doesn't pull it back - and the brake
// held in-betweens off in busy views at 60 fps.)
static void present_brake(uint64_t now, bool interp) {
    static uint64_t last, last_brake;
    static uint8_t slow[120];
    static int n, nslow, backoff = 120;
    if (last && now > last) {
        double d = (double)(now - last);
        game_period += ((d < 8e6 ? 8e6 : d > 50e6 ? 50e6 : d) - game_period) * 0.05;
        static int shots = -1;  // (ODYSSEY_INTERP_SHOT: saving images makes frames late - no brake for the series)
        if (shots < 0) shots = getenv("ODYSSEY_INTERP_SHOT") != NULL;
        if (interp && !shots) {
            int k = n++ % 120;
            nslow += (d > 18e6) - slow[k];
            slow[k] = d > 18e6;
            if (nslow >= 12) {
                brake_until = total_frames + (uint64_t)backoff;
                LOG("[interp] the game missed its vsync in %d of its last %d frames with in-betweens: paused for %d frames\n",
                    nslow, n < 120 ? n : 120, backoff);
                backoff = backoff * 2 > 1200 ? 1200 : backoff * 2;
                memset(slow, 0, sizeof slow);
                n = nslow = 0;
                last_brake = total_frames;
            }
        }
    }
    last = now;
    if (backoff > 120 && total_frames > last_brake + 1800) backoff = 120;
}

static void backend_present_locked(NvnTexture *t, bool handed);
void backend_present(NvnTexture *t) {
    plat_heap_check("present");
    // (a frame the interpolation thread presents needs no render lock here: its replay may hold it, and the game's
    // next frame starts without waiting for that - lists it submits meanwhile wait, interp_wait_replay)
    bool handed = fd.done && fd.frame == total_frames && fd.game_w;
    if (!handed) vk_render_lock();
    backend_present_locked(t, handed);
    if (!handed) { extern void vk_tex_prefetch(void); vk_tex_prefetch(); }
    if (!handed) vk_astc_gpu_check_run();  // (ODYSSEY_ASTC_CHECK only)
    if (!handed) vk_render_unlock();
}
static void backend_present_locked(NvnTexture *t, bool handed) {
    extern void debug_tex_trace(const char *what);
    if (!handed) debug_tex_trace("present");  // (ODYSSEY_TEX_TRACE: exec.c)
    g_last_presented = t;
    uint64_t present_ns = plat_now_ns();
    if (!handed) {
        vk_tex(t, true);
        vk_end_pass();
        if (!fd.done || fd.frame != total_frames) frame_decide(t, present_ns);
    }
    fd.done = false;
    TexBackend *b = fd.b, *game_img = fd.game_img;
    b->presented = true;
    present_brake(present_ns, fd.game_w != NULL);
    static const float black[4] = {0, 0, 0, 1};
    uint64_t t0 = SDL_GetTicksNS();
    pcache_autosave();
    float v[4], pv[8];
    memcpy(v, fd.v, sizeof v);
    memcpy(pv, fd.pv, sizeof pv);
    {   // ODYSSEY_TEST_REMAKE_AT=<frame>[,<frame>...] (tests): the swapchain remade there, as a VSync change does;
        // ODYSSEY_TEST_FG_OFF_AT=<frame>: frame generation turned off there (the setting), on again 300 frames later
        static const char *ra = (const char *)1, *fo = (const char *)1;
        if (ra == (const char *)1) ra = getenv("ODYSSEY_TEST_REMAKE_AT"), fo = getenv("ODYSSEY_TEST_FG_OFF_AT");
        for (const char *p = ra; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)total_frames) { LOG("[test] swapchain remake at frame %ld\n", at); g_need_recreate = true; }
            if (*p == ',') p++; else break;
        }
        if (fo && (atol(fo) == (long)total_frames || atol(fo) + 300 == (long)total_frames)) {
            g_settings.frame_gen = atol(fo) != (long)total_frames ? FG_FSR : FG_OFF;
            LOG("[test] frame generation %s at frame %llu\n", g_settings.frame_gen ? "on" : "off", (unsigned long long)total_frames);
            g_need_recreate = true;
        }
    }
    static long menu_at = -1;  // ODYSSEY_MENU_AT=<frame> (tests): the menu opens there
    if (menu_at < 0) menu_at = getenv("ODYSSEY_MENU_AT") ? atol(getenv("ODYSSEY_MENU_AT")) : 0;
    if (menu_at && (long)total_frames + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
    bool have = false, async_done = false;
    // The present on the presenter thread (ODYSSEY_ASYNC_PRESENT=1/0; on by default on Linux): the window's picture is
    // composed here into an image of its own and submitted, and the presenter thread acquires a window image, copies it
    // in (with the FPS counter / performance overlay) and presents it - off the render thread. On the Steam Deck the
    // acquire and the present took ~4.5 ms each there (waits in the window system, not the GPU): ~9 ms of every frame
    // in which the render thread couldn't run the next one's lists, 41 FPS in Bonneton's town instead of 60. With the
    // menu open (its input and fonts go with the render lock) or a debug frame (F9): the direct way below.
    static int async_present = -1;
    if (async_present < 0) {
        const char *e = getenv("ODYSSEY_ASYNC_PRESENT");
#ifdef __linux__
        async_present = !e || atoi(e) != 0;
#else
        async_present = e && atoi(e) != 0;
#endif
        if (async_present) LOG("[vk] presents on their own thread (ODYSSEY_ASYNC_PRESENT=0: on the render thread)\n");
    }
    extern bool g_debug_frame;
    if (async_present && !handed && !fd.game_w && !fd.skip && fd.placed && menu_overlay_lockfree() && !g_debug_frame) {
        static TexBackend aw[4];  // (four: the presenter may still be copying an earlier one)
        static int ai;
        TexBackend *w = &aw[ai = (ai + 1) % 4];
        if (compose(w, b, v, pv)) {
            renderer_submit();
            presenter_queue(w, plat_now_ns(), 2 * (total_frames + 1), "game image (async)");
            async_done = true;
        }
    }
    if (!async_done && !fd.game_w && !fd.skip && fd.placed) {
        have = vk_frame_begin(black);
        uint64_t nd = SDL_GetTicksNS() - t0;
        if (nd > 30000000) {
            static int warned;
            if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
        }
    }
    if (have) {
        VkCommandBuffer cb = vk_cmd();
        vk_checkpoint(cb, "present draw %p %ux%u -> window %.0fx%.0f", (void *)b->image, b->width, b->height, v[2], v[3]);
        vk_present_draw(cb, b, v[0], v[1], v[2], v[3], pv);
        vk_checkpoint(cb, "menu overlay");
        vk_queue_lock();  // (ImGui's first frame uploads its fonts through the queue)
        menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        vk_queue_unlock();
        static long win_shot = -1, shot_count = 1, shot_index;
        if (win_shot < 0) {
            const char *spec = getenv("ODYSSEY_WINDOW_SHOT");
            win_shot = 0;
            if (spec) sscanf(spec, "%ld:%ld", &win_shot, &shot_count);
        }
        bool shot = win_shot && (long)total_frames + 1 >= win_shot && shot_index < shot_count;
        char shot_path[1024];
        if (shot) {
            if (shot_count == 1) snprintf(shot_path, sizeof shot_path, "%s",
                getenv("ODYSSEY_WINDOW_SHOT_PATH") ? getenv("ODYSSEY_WINDOW_SHOT_PATH") : "build/window.png");
            else snprintf(shot_path, sizeof shot_path, "build/window_%02ld.png", shot_index);
            shot_index++;
        }
        {   // (a test script's "frame:shot:<name>": runtime/hle/input.c)
            extern char g_window_shot_request[256];
            if (!shot && g_window_shot_request[0]) {
                snprintf(shot_path, sizeof shot_path, "%s", g_window_shot_request);
                g_window_shot_request[0] = 0;
                shot = true;
            }
        }
        extern bool g_debug_frame;
        if (g_debug_frame && !shot) {  // (a debug frame, F9: the window as shown, next to its passes and probe lines)
            snprintf(shot_path, sizeof shot_path, "build/debug_window.png");
            shot = true;
        }
        g_present_order = 2 * (total_frames + 1);  // (the present lock: vk_frame_begin's, until vk_frame_end)
        vk_frame_end(shot ? shot_path : NULL);
        if (getenv("ODYSSEY_LOG_PRESENTS")) LOG("[present] %.3f game image (direct)\n", plat_now_ns() / 1e6);
        if (shot && shot_index == shot_count && getenv("ODYSSEY_EXIT_AFTER_SHOT")) { extern void sampler_flush(void); sampler_flush(); SL_TEST_EXIT(); fflush(stderr); _exit(0); }
    } else if (!handed && !async_done) {  // (an async present submitted already)
        renderer_submit();
    }
    odyssey_pose_render_frame(NULL);
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    vk_prof_frame(total_frames, SDL_GetTicksNS() - t0);
    frames++;
    if (fd.skip) held_frames++;
    total_frames++;
    g_frame_counter++;
    extern void vk_pace_frame(void);
    vk_pace_frame();  // (exec.c: the game's 30 fps mode when frames don't fit 60)
    extern void input_frame(uint64_t frame);
    input_frame(total_frames);
    static long slow_ms = -1;
    if (slow_ms < 0) slow_ms = getenv("ODYSSEY_SLOW_FRAME_MS") ? atol(getenv("ODYSSEY_SLOW_FRAME_MS")) : 0;
    static long slow_until = -1;  // ODYSSEY_SLOW_FRAME_UNTIL=<frame>: the stall ends there (frame pacing recovery tests)
    if (slow_until < 0) slow_until = getenv("ODYSSEY_SLOW_FRAME_UNTIL") ? atol(getenv("ODYSSEY_SLOW_FRAME_UNTIL")) : 0;
    if (slow_ms > 0 && total_frames > 2600 && (!slow_until || (long)total_frames < slow_until)) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(total_frames);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (ODYSSEY_REBOOT_AT)
    reboot_test_frame(total_frames);
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("ODYSSEY_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    extern bool g_debug_frame;
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = getenv("ODYSSEY_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // ODYSSEY_DEBUG_FRAMES=<n> (with ODYSSEY_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = getenv("ODYSSEY_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
    {   // ODYSSEY_GPUPROF_AT=<frame>[,<frame>...]: GPU time per pass of those frames ("[gpuprof]" in the log)
        extern bool g_gpuprof_frame;
        extern void vk_gpuprof_report(void);
        if (g_gpuprof_frame) { g_gpuprof_frame = false; vk_gpuprof_report(); }
        const char *e = getenv("ODYSSEY_GPUPROF_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)total_frames) { g_gpuprof_frame = true; break; }
            if (*p == ',') p++; else break;
        }
    }
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", (unsigned long long)total_frames);
    {   // ODYSSEY_SHOT_AT=<frame>,<frame>,... (ascending): the game image at those frames -> build/frames/frame_N.png
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("ODYSSEY_SHOT_AT"); if (next) mkdir("build/frames", 0755); }
        while (next && *next) {
            long at = strtol(next, (char **)&next, 10);
            if ((long)total_frames < at) { while (next > getenv("ODYSSEY_SHOT_AT") && next[-1] != ',') next--; break; }
            if ((long)total_frames == at) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/frame_%05llu.png", (unsigned long long)total_frames);
                vk_render_lock();  // (a frame the interpolation thread presents comes here without it)
                renderer_save_texture_tex(game_img ? game_img : b, path);
                vk_render_unlock();
            }
            if (*next == ',') next++;
        }
    }
    static long shot_from = -1;
    if (shot_from < 0) { const char *e = getenv("ODYSSEY_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && total_frames % shot_every == 0 && (long)total_frames >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png",
                 (unsigned long long)total_frames);
        vk_render_lock();  // (a frame the interpolation thread presents comes here without it)
        renderer_save_texture_tex(game_img ? game_img : b, path);
        vk_render_unlock();
        // a series (SCREENSHOT_FROM) runs on until ODYSSEY_WINDOW_SHOT exits
        if (getenv("ODYSSEY_EXIT_AFTER_SHOT") && !shot_from) { extern void sampler_flush(void); sampler_flush(); SL_TEST_EXIT(); fflush(stderr); _exit(0); }
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;
    if (fps_iv < 0) fps_iv = getenv("ODYSSEY_FPS_INTERVAL") ? atof(getenv("ODYSSEY_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    {   // ODYSSEY_LOG_SLOW_FRAMES=<ms>: each present gap longer than that (hitches; next to [shtime] lines)
        static double slow = -2;
        if (slow == -2) slow = getenv("ODYSSEY_LOG_SLOW_FRAMES") ? atof(getenv("ODYSSEY_LOG_SLOW_FRAMES")) : -1;
        if (slow >= 0 && prev_present > 0 && (now - prev_present) * 1000.0 >= slow)
            LOG("[slow] frame %llu: %.0f ms since the last present\n", (unsigned long long)total_frames, (now - prev_present) * 1000.0);
    }
    prev_present = now;
    if (now - last_fps_time >= fps_iv) {
        if (held_frames)
            LOG("[vk] shader builds held %llu of %llu frames through frame %llu\n",
                (unsigned long long)held_frames, (unsigned long long)frames, (unsigned long long)total_frames);
        if (last_fps_time > 0) {  // ("level -": the field the log tools expect)
            uint64_t st0 = plat_now_ns();
            const char *stats = renderer_stats();
            uint64_t st1 = plat_now_ns();
            // (one write: stderr is unbuffered, and a log redirected to a pipe or file by another program took each
            // character of this long line as a write of its own - 60 ms every 2 s on the presenting thread)
            char line[2048];
            int ln = snprintf(line, sizeof line, "[vk] t=%.1f frame %llu %.1f fps max %.0fms level - (%s)\n", now,
                              (unsigned long long)total_frames, frames / (now - last_fps_time), max_gap * 1000.0, stats);
            if (ln > 0) fwrite(line, 1, ln < (int)sizeof line ? (size_t)ln : sizeof line - 1, stderr);
            {   // A plain status line a minute (players' logs: where, how smooth, how much memory - at a glance)
                static double minute_t0, minute_low = 1e9, minute_worst;
                static uint64_t minute_f0;
                double fps = frames / (now - last_fps_time);
                if (fps < minute_low) minute_low = fps;
                if (max_gap > minute_worst) minute_worst = max_gap;
                if (!minute_t0) minute_t0 = now, minute_f0 = total_frames;
                if (now - minute_t0 >= 60.0) {
                    extern const char *odyssey_stage_name(void);
                    LOG("[status] %.0f min: %s, %.1f FPS (lowest 2 s: %.1f), worst frame %.0f ms, memory %.1f GB in RAM, "
                        "%.1f GB committed\n", now / 60.0, odyssey_stage_name(), (total_frames - minute_f0) / (now - minute_t0),
                        minute_low, minute_worst * 1000.0, plat_resident_bytes() / 1073741824.0, plat_private_bytes() / 1073741824.0);
                    minute_t0 = now, minute_f0 = total_frames, minute_low = 1e9, minute_worst = 0;
                }
            }
            uint64_t st2 = plat_now_ns();
            if (st2 - st0 > 5000000ull) {  // (the stats line itself: memory queries, the log write)
                static int nlog;
                if (nlog++ < 10) LOG("[vk] that stats line took %.1f ms (%.1f ms to gather)\n", (st2 - st0) / 1e6, (st1 - st0) / 1e6);
            }
        }
        max_gap = 0;
        frames = held_frames = 0;
        last_fps_time = now;
    }
}

// ================================================================== UI thread
void vk_exec_init(void);

// exit() from any thread: let the GPU finish, keep the pipeline cache, then end the process without the DLL
// teardown (driver / validation-layer threads can stall it for minutes while the game's threads still run).
static void quick_exit_handler(void) {
    static int trace = -1;
    if (trace < 0) trace = getenv("ODYSSEY_VK_TRACE") != NULL;
    { extern void sampler_flush(void); sampler_flush(); }  // (a sampling session still going: written out)
    if (trace) LOG("[vk] exit: waiting for the GPU\n");
    if (g_vk.dev) {
        vk_queue_lock();
        device_wait_idle();
        vk_queue_unlock();
        // (NVIDIA Streamline is NOT shut down here: the render thread still calls its Reflex / latency markers each
        // frame, and slShutdown unloads those plugins - the next call jumped into freed code, a crash at every quit
        // with DLSS frame generation on, issue #70. Nothing needs it: plat_terminate below ends every thread at once
        // without the DLL teardown, the device is never destroyed.)
        if (trace) LOG("[vk] exit: saving the pipeline cache\n");
        vk_pipeline_cache_save();
        vk_pipeline_recipes_save();
        extern void vk_drawn_programs_save(void);
        vk_drawn_programs_save();
    }
    vk_start_ok();
    if (trace) LOG("[vk] exit: terminating\n");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
}

// Quitting: the quick exit above first, not exit()'s other handlers. exit() runs the handlers registered after ours
// first - the Vulkan layers' (Steam's fossilize layer on a Steam Deck) tore down their state while the shader workers
// still called through them: a crash at every quit there ("signal 11 accessing 0x18" in libVkLayer_steam_fossilize,
// then std::system_error). The game's own quit requests and the window's close button come here.
void vk_exit_now(void) {
    quick_exit_handler();  // (ends the process on Windows and Linux)
    exit(0);
}

void ui_window_init(void) {
    if (window) return;
    atexit(quick_exit_handler);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    settings_load();
    updater_auto_check();  // (in the background; the menu's Updates page shows it)
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    int win_w = 1280, win_h = 720;
    if (getenv("ODYSSEY_WINDOW")) sscanf(getenv("ODYSSEY_WINDOW"), "%dx%d", &win_w, &win_h);
    SDL_WindowFlags wflags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (getenv("ODYSSEY_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
    {   // scripted test runs (user input ignored: runtime/hle/input.c input_user_blocked) open without taking the
        // keyboard focus from whatever the player is doing
        const char *v = getenv("ODYSSEY_NO_USER_INPUT");
        bool scripted = v ? atoi(v) != 0 : (getenv("ODYSSEY_INPUT") || getenv("ODYSSEY_DOORJUMP_TEST") || getenv("ODYSSEY_PLACE") ||
                                            getenv("ODYSSEY_HOST_INPUT") || getenv("ODYSSEY_BIND_TEST"));
        if (scripted) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    window = SDL_CreateWindow(RT_APP_NAME, win_w, win_h, wflags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    rt_window_icon(window);
    // the Fullscreen setting applies from the first frame on; test windows stay windows
    if (!getenv("ODYSSEY_WINDOW") && !getenv("ODYSSEY_HIDDEN")) menu_apply_window_requests(window);
    create_instance();
    bool surface = false;
#ifdef _WIN32
    if (vk_sl_active()) {
        PFN_vkCreateWin32SurfaceKHR p = (PFN_vkCreateWin32SurfaceKHR)vk_sl_instance_proc(g_vk.inst, "vkCreateWin32SurfaceKHR");
        HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        VkWin32SurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                          .hinstance = GetModuleHandleW(NULL), .hwnd = hwnd};
        surface = p && hwnd && p(g_vk.inst, &si, NULL, &g_vk.surface) == VK_SUCCESS;
    }
#endif
    if (!surface && !SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
#ifdef _WIN32
    {   // NVIDIA NGX's start (the DLSS check) takes ~1.7 s of every start (RTX 4090, 2026-11-16): on a thread of its own,
        // the game starts meanwhile - DLSS becomes available when it is done (vk_temporal_enabled reads its flag).
        // ODYSSEY_DLSS_PROBE_SYNC=1: before the game, as before.
        static pthread_t probe;
        if (getenv("ODYSSEY_DLSS_PROBE_SYNC") || pthread_create(&probe, NULL, vk_dlss_probe_thread, NULL))
            vk_dlss_probe_run();
        else pthread_detach(probe);
    }
    vk_astc_gpu_selftest();  // (ODYSSEY_ASTC_SELFTEST only: the GPU and CPU ASTC decoders on random blocks)
    extern void vk_fsr3_init(void);
    vk_fsr3_init();
#endif
    create_swapchain();
    vk_exec_init();
    vk_shaders_init();
    vk_pipeline_prewarm(!recovering_pipeline_cache);
    vk_start_ok();
}

SDL_Window *ui_window(void) { return window; }

// Show the progress of a full shader preparation round. Closing the window leaves the cache entries already saved.
static int prep_run(SDL_Renderer *r, uint64_t t0, const char *pass, const char *log) {
    char exe[1024];
    if (!plat_exe_path(exe, sizeof exe)) return -2;
    const char *argv[] = {exe, "--prepare-shaders", pass, "--data-root", rt_data_root(), NULL};
    PlatProc pr = plat_spawn(argv, log, true);
    if (!pr) { LOG("[prep] the preparation's process didn't start\n"); return -2; }
    int code = -1, done = 0, total = 0, phase = !strcmp(pass, "recorded") ? 2 : 1;
    while ((code = plat_proc_wait(pr, false)) < 0) {
        SDL_Event ev;
        bool skip = false;
        while (SDL_PollEvent(&ev)) skip |= ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        if (skip) { plat_proc_kill(pr); LOG("[prep] skipped (window closed)\n"); return -1; }
        FILE *f = fopen("cache/prepare.progress", "r");
        if (f) { int d = 0, n = 0, ph = 1; if (fscanf(f, "%d %d %d", &d, &n, &ph) >= 2 && n > 0) done = d, total = n, phase = ph; fclose(f); }
        if (r) {
            float sec = (float)((plat_now_ns() - t0) / 1e9);
            const char *phase_text = phase == 0 ? "Translating shaders" :
                                     phase == 2 ? "Compiling pipelines" : "Compiling shaders";
            extern void prep_window_draw(SDL_Renderer *r, const char *phase, int done, int total, float sec);
            prep_window_draw(r, phase_text, done, total, sec);  // (the menu's look and font: runtime/ui/prep_window.mm)
            static const char *shot = (const char *)1;  // (test, ODYSSEY_PREP_SHOT=<file>: the window as a BMP, once)
            if (shot == (const char *)1) shot = getenv("ODYSSEY_PREP_SHOT");
            if (shot && done > 0) {
                SDL_Surface *s = SDL_RenderReadPixels(r, NULL);
                bool saved = s && SDL_SaveBMP(s, shot);
                LOG("[prep] window shot %s: %s\n", shot, saved ? "saved" : SDL_GetError());
                if (s) SDL_DestroySurface(s);
                shot = NULL;
            }
            SDL_RenderPresent(r);
        }
        SDL_Delay(50);
    }
    LOG("[prep] %s: %d of %d (exit %d)\n", pass, done, total, code);
    return code;
}

// The menu's Compile shaders (Video, Shaders): a full pass at the next start, asked for by this file
#define PREPARE_REQUEST "cache/prepare.request"
bool vk_prepare_requested(void) {
    struct stat st;
    return stat(PREPARE_REQUEST, &st) == 0;
}
void vk_prepare_request(bool on) {
    if (!on) { remove(PREPARE_REQUEST); return; }
    mkdir("cache", 0755);
    FILE *f = fopen(PREPARE_REQUEST, "w");
    if (f) { fprintf(f, "compile all shaders at the next start (the menu's Compile shaders)\n"); fclose(f); }
    LOG("[prep] full shader preparation %s for the next start\n", f ? "requested" : "could not be requested");
}

// Do a full pass on a fresh profile. On later starts the existing cache is reused, even after an interrupted
// preparation or a new executable. ODYSSEY_PRECOMPILE=1 opts into a full pass once per executable; the menu's
// Compile shaders (cache/prepare.request) into one at the next start, whatever was done before.
// Whether this start has a full preparation due (vk_prepare_before_start's checks, without doing it): the configuration
// page (runtime/ui/setup.mm) asks the player then and runs it on its own page, with a progress bar.
static bool g_prepare_skipped;  // (the player said Later: not this start)
// What a prepared cache was made for: the GPU, its driver and this program's version (the PREPARED_MARK's second
// line). A driver update makes the driver refuse the saved pipeline binaries, and a new version may translate shaders
// differently: either way the cache no longer matches and every effect would build while playing - the stutters
// players saw after updating. A different identity (or a mark from before it was kept) makes the preparation due again.
static void prep_identity(char out[128]) {
    static char id[128];
    static bool done;
    if (!done) {  // (before the renderer's own device: a short-lived instance, the GPU chosen as pick_device does)
        done = true;
        VkPhysicalDeviceProperties best = g_vk.props;
        if (!best.vendorID) {
            VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
            VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
            VkInstance inst;
            if (vkCreateInstance(&ci, NULL, &inst) == VK_SUCCESS) {
                uint32_t n = 0;
                vkEnumeratePhysicalDevices(inst, &n, NULL);
                VkPhysicalDevice pd[16];
                if (n > 16) n = 16;
                vkEnumeratePhysicalDevices(inst, &n, pd);
                int want = getenv("ODYSSEY_GPU") ? atoi(getenv("ODYSSEY_GPU")) : -1, best_score = -1;
                for (uint32_t i = 0; i < n; i++) {
                    VkPhysicalDeviceProperties p;
                    vkGetPhysicalDeviceProperties(pd[i], &p);
                    if (p.apiVersion < VK_API_VERSION_1_3) continue;
                    int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
                    if ((int)i == want) score = 100;
                    if (score > best_score) best_score = score, best = p;
                }
                vkDestroyInstance(inst, NULL);
            }
        }
        if (best.vendorID)
            snprintf(id, sizeof id, "gpu %04x:%04x driver %u app %s", best.vendorID, best.deviceID, best.driverVersion, APP_VERSION);
    }
    memcpy(out, id, 128);
}
static void prep_mark_write(const char *key) {
    char id[128];
    prep_identity(id);
    FILE *f = fopen(PREPARED_MARK, "w");
    if (f) { fprintf(f, "%s\n%s\n", key, id); fclose(f); }
}
static bool prepare_due(bool log, char key[64]) {
    key[0] = 0;
    if (g_prepare_skipped) return false;
    const char *e = getenv("ODYSSEY_PREPARE");
    if (e && !atoi(e)) return false;
    const char *pe = getenv("ODYSSEY_PRECOMPILE");
    if (pe && !atoi(pe)) return false;
    struct stat st;
    // A recovery boot runs with fresh driver caches. The cache guard clears the completion marker so the
    // next normal boot prepares the shaders again.
    if (cache_start_interrupted()) return false;
    bool requested = vk_prepare_requested();
    bool cached = vk_spirv_cache_available();
    // (PREPARED_MARK: a full pass finished, or was closed by the player - written below. A cache without it was
    // only filled while playing, e.g. a test run with ODYSSEY_PREPARE=0: the full pass still runs once.)
    bool prepared = stat(PREPARED_MARK, &st) == 0;
    bool stale = false;
    if (cached && prepared && !pe && !requested) {
        char want[128], have_id[128] = "", line[128];
        prep_identity(want);
        FILE *f = fopen(PREPARED_MARK, "r");
        if (f) {
            if (fgets(line, sizeof line, f) && fgets(have_id, sizeof have_id, f)) have_id[strcspn(have_id, "\r\n")] = 0;
            fclose(f);
        }
        if (!want[0] || !strcmp(have_id, want)) {
            if (log) LOG("[prep] shader cache found; using quick startup\n");
            return false;
        }
        stale = true;
        if (log) LOG("[prep] the shader cache was prepared for %s; now %s: full pass\n", have_id[0] ? have_id : "an earlier version", want);
    }
    if (log && cached && !prepared) LOG("[prep] shader cache never fully prepared: full pass\n");
    if (log && requested) LOG("[prep] full shader preparation (asked for in the menu)\n");
    char exe[1024], have[64] = "";
    if (plat_exe_path(exe, sizeof exe) && stat(exe, &st) == 0) snprintf(key, 64, "%lld %lld", (long long)st.st_size, (long long)st.st_mtime);
    if (cached && key[0] && !requested && !stale) {
        FILE *f = fopen(PREPARED_MARK, "r");
        if (f) { if (!fgets(have, sizeof have, f)) have[0] = 0; fclose(f); }
        have[strcspn(have, "\r\n")] = 0;
        if (!strcmp(have, key)) return false;
    }
    return true;
}
bool vk_gpu_is_nvidia(void) { return g_vk.props.vendorID == 0x10de; }  // (the menu: Reflex, Multithreaded rendering Auto)
bool vk_prepare_due(void) { char key[64]; return prepare_due(false, key); }
void vk_prepare_skip_this_start(void) { g_prepare_skipped = true; LOG("[prep] shader preparation put off (Later)\n"); }

void vk_prepare_before_start(void) {
    char key[64];
    if (!prepare_due(true, key)) return;
    mkdir("cache", 0755);
    remove("cache/prepare.progress");
    uint64_t t0 = plat_now_ns();
    LOG("[prep] precompiling the game's shaders\n");
    SDL_Window *w = SDL_Init(SDL_INIT_VIDEO) ? SDL_CreateWindow(RT_APP_NAME, 640, 210, getenv("ODYSSEY_HIDDEN") ? SDL_WINDOW_HIDDEN : 0) : NULL;
    SDL_Renderer *r = w ? SDL_CreateRenderer(w, "software") : NULL;
    if (w) rt_window_icon(w);
    int code = 0;
    for (int round = 0; round < 1000 && (code = prep_run(r, t0, "all", "cache/prepare.log")) == 2; round++)
        remove("cache/prepare.progress");
    if (code == 0 || code == 3) {
        for (int round = 0; round < 1000; round++) {
            remove("cache/prepare.progress");
            code = prep_run(r, t0, "recorded", "cache/prepare-pipelines.log");
            if (code != 2) break;
        }
    }
    if ((code == 0 || code == -1) && key[0]) {  // (done, or skipped by closing its window: not asked again)
        prep_mark_write(key);
    }
    if (code == 0 || code == -1) remove(PREPARE_REQUEST);  // (done, or skipped by closing its window)
    { extern void prep_window_close(void); prep_window_close(); }
    if (r) SDL_DestroyRenderer(r);
    if (w) SDL_DestroyWindow(w);
    LOG("[prep] before the start: %.1f s\n", (plat_now_ns() - t0) / 1e9);
}

// The setup's last step (runtime/ui/setup.mm): the same full preparation as vk_prepare_before_start - every shader of
// the game's archives, then every recorded (or seeded) pipeline - from the setup's worker thread, its progress to the
// setup page instead of a window of its own. Marks the cache done, so later starts go straight to the game. cancel
// (may be NULL): stops it (the entries done so far stay). 0 = done.
int vk_prepare_install(void (*progress)(int phase, int done, int total), volatile int *cancel) {
    char exe[1024], key[64] = "";
    struct stat st;
    if (!plat_exe_path(exe, sizeof exe)) return -2;
    if (stat(exe, &st) == 0) snprintf(key, sizeof key, "%lld %lld", (long long)st.st_size, (long long)st.st_mtime);
    mkdir("cache", 0755);
    static const char *const passes[] = {"all", "recorded"};
    static const char *const logs[] = {"cache/prepare.log", "cache/prepare-pipelines.log"};
    int code = 0;
    for (int pass = 0; pass < 2 && code == 0; pass++) {
        for (int round = 0; round < 1000; round++) {
            remove("cache/prepare.progress");
            const char *argv[] = {exe, "--prepare-shaders", passes[pass], "--data-root", rt_data_root(), NULL};
            PlatProc pr = plat_spawn(argv, logs[pass], true);
            if (!pr) { LOG("[prep] the preparation's process didn't start\n"); return -2; }
            while ((code = plat_proc_wait(pr, false)) < 0) {
                if (cancel && *cancel) { plat_proc_kill(pr); LOG("[prep] install pass cancelled\n"); return -1; }
                FILE *f = fopen("cache/prepare.progress", "r");
                int d = 0, n = 0, ph = pass ? 2 : 1;
                if (f) {
                    if (fscanf(f, "%d %d %d", &d, &n, &ph) >= 2 && n > 0 && progress) progress(ph, d, n);
                    fclose(f);
                }
                SDL_Delay(100);
            }
            LOG("[prep] install pass %s round %d: exit %d\n", passes[pass], round, code);
            if (code != 2) break;  // 2: a memory budget was reached, the next round goes on where it stopped
        }
        if (pass == 0 && code == 3) code = 0;  // (3: done, some programs unavailable on this GPU)
    }
    if (code == 0 && key[0]) {
        prep_mark_write(key);
        remove(PREPARE_REQUEST);
    }
    return code;
}

// The shader preparation's process (exec.c vk_prepare_main): the game's instance, GPU and device - pipeline binaries
// only fit those - on a hidden window, without a swapchain.
void vk_headless_init(void) {
    settings_load();
    if (!SDL_Init(SDL_INIT_VIDEO)) FATAL("SDL_Init: %s\n", SDL_GetError());
    window = SDL_CreateWindow(RT_APP_NAME, 64, 64, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    g_vk.headless = true;
    create_instance();
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
}

void backend_init(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
    LOG("[vk] %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
}

#ifdef __linux__
// Called at the first SetHeapSize, before the game can use its heap. Waiting
// here lets the UI finish device creation without relocating live guest data.
void backend_prepare_guest_heap(uint64_t size) {
    const char *e = getenv("ODYSSEY_VK_PLACED_HEAP");
    const char *off = getenv("ODYSSEY_VK_NO_GMEM");
    if ((e && !atoi(e)) || !size || (off && atoi(off))) return;
    backend_init();
    extern bool vk_prepare_placed_heap(void);
    if (!g_vk.placed_heap || !vk_prepare_placed_heap())
        LOG("[vk] placed guest heap unavailable; retaining host import\n");
}
#endif

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
void backend_reset(void) {
    odyssey_pose_render_frame(NULL);
    if (!g_vk.dev) return;
    vkDeviceWaitIdle(g_vk.dev);
    extern void vk_textures_reset(void);
    extern void vk_exec_reset(void);
    vk_textures_reset();
    vk_exec_reset();
    g_last_presented = NULL;
}
void backend_guest_memory_reset(void) {}  // (exec.c re-imports the heap on first use)
void backend_clear_window(void) {
    static const float black[4] = {0, 0, 0, 1};
    if (g_vk.dev && vk_frame_begin(black)) vk_frame_end(NULL);
}

int ui_run(void) {
    ui_window_init();
    atomic_store(&ui_ready, true);
    for (;;) {
        if (rt_reboot_requested()) return rt_reboot_requested();  // (runtime/main.c tears the game down)
        SDL_Event e;
        unsigned event_count = 0;
        while (ui_next_event(&e, &event_count)) {
            if (e.type == SDL_EVENT_QUIT ||
                (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window))) {
                LOG("window closed\n");
                // No present after this (the queue lock: one under way finishes first): hiding the window ends its
                // surface under Steam's gamescope, and the presenter and render threads went on presenting while the
                // exit waited for the GPU and saved the caches - gamescope's layer aborted ("surface already
                // destroyed"), a crash report at every quit on a Steam Deck.
                atomic_store(&g_no_present, true);
                vk_queue_lock();
                vk_queue_unlock();
                SDL_HideWindow(window);  // Return the desktop immediately, including in exclusive fullscreen.
                vk_exit_now();
            }
            extern bool input_user_blocked(const SDL_Event *e);
            if (input_user_blocked(&e)) continue;  // scripted test runs: the real keyboard / mouse / pads do nothing (as metal.m)
            if (menu_handle_event(&e)) continue;
            input_handle_event(&e);
        }
        menu_apply_window_requests(window);
        extern void input_mouse_window(SDL_Window *w);
        input_mouse_window(window);  // (the mouse camera: the pointer held while the game has it)
        {   // ODYSSEY_RESIZE_TEST=<frame>:<w>x<h>[,<frame>:<w>x<h>...] (tests): the window changes size at those frames
            static const char *next;
            static bool init;
            if (!init) { init = true; next = getenv("ODYSSEY_RESIZE_TEST"); }
            long at;
            int w, h, n = 0;
            if (next && *next && sscanf(next, "%ld:%dx%d%n", &at, &w, &h, &n) == 3 && (long)total_frames >= at) {
                LOG("[vk] resize test: %dx%d at frame %llu\n", w, h, (unsigned long long)total_frames);
                SDL_SetWindowSize(window, w, h);
                next += n;
                if (*next == ',') next++;
            }
        }
    }
}
