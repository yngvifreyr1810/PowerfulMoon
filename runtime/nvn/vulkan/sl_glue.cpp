// NVIDIA Streamline on our Vulkan renderer (Windows): DLSS Frame Generation and Reflex. See sl_glue.h.
// Streamline is loaded at run time (sl.interposer.dll next to the exe, its Authenticode signature checked - NVIDIA's),
// in manual-hooking mode: we call its proxies for the functions it must see (instance, device, surface, swapchain,
// present, device wait idle) and Vulkan's own for the rest. Over-the-air plugin downloads are off.
// Its exported API (slInit ...) is resolved from the DLL; the feature functions (DLSS-G, Reflex, PCL) through
// slGetFeatureFunction, as Streamline's own inline wrappers do.
#ifdef _WIN32
#include "sl_glue.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "../../../third_party/streamline/include/sl.h"
#include "../../../third_party/streamline/include/sl_dlss_g.h"
#include "../../../third_party/streamline/include/sl_reflex.h"
#include "../../../third_party/streamline/include/sl_pcl.h"
#include "../../../third_party/streamline/include/sl_security.h"

extern "C" {
#include "../../settings.h"
}
#define SLOG(...) fprintf(stderr, __VA_ARGS__)  // (= LOG, runtime/rt.h)

static struct {
    HMODULE mod;
    bool active, dlssg, reflex;
    PFun_slInit *init;
    PFun_slShutdown *shutdown;
    PFun_slIsFeatureSupported *supported;
    PFun_slSetTagForFrame *tag;
    PFun_slSetConstants *constants;
    PFun_slGetFeatureFunction *feature;
    PFun_slGetNewFrameToken *token;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
    uint64_t token_frame;
    sl::FrameToken *cur;
    uint64_t scene_frame;  // the last frame with tags (DLSS-G only on those)
    int dlssg_on;          // the mode last set (-1: none yet)
    uint32_t mw, mh, dw, dh;
    uint32_t backbuffers;
    VkFormat color_format;
} sl_;
extern "C" void vk_sl_swapchain(uint32_t images, VkFormat format) { sl_.backbuffers = images, sl_.color_format = format, sl_.dlssg_on = -1; }

// (the exported entry points Streamline's inline helpers call: forwarded to the DLL)
extern "C" sl::Result slGetFeatureFunction(sl::Feature feature, const char *functionName, void *&function) {
    return sl_.feature ? sl_.feature(feature, functionName, function) : sl::Result::eErrorNotInitialized;
}

static bool exe_dir(wchar_t *out, size_t n) {
    DWORD len = GetModuleFileNameW(NULL, out, (DWORD)n);
    if (!len || len >= n) return false;
    wchar_t *s = wcsrchr(out, L'\\');
    if (!s) return false;
    s[1] = 0;
    return true;
}

static void sl_log(sl::LogType type, const char *msg) {
    static int all = -1;
    if (all < 0) all = getenv("ODYSSEY_SL_LOG") != NULL;
    if (all || type != sl::LogType::eInfo) SLOG("[streamline] %s", msg);
}

extern "C" bool vk_sl_init(void) {
    const char *e = getenv("ODYSSEY_SL");
    bool want = e && *e ? atoi(e) != 0 : g_settings.frame_gen == FG_DLSS || g_settings.low_latency != 0;
    if (!want) return false;
    wchar_t dir[1024], path[1100];
    if (!exe_dir(dir, 1024)) return false;
    swprintf(path, 1100, L"%lssl.interposer.dll", dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) { SLOG("[streamline] runtime absent (sl.interposer.dll)\n"); return false; }
    if (!sl::security::verifyEmbeddedSignature(path)) { SLOG("[streamline] sl.interposer.dll is not signed by NVIDIA: not loaded\n"); return false; }
    sl_.mod = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!sl_.mod) return false;
#define GET(m, n) sl_.m = (decltype(sl_.m))(void *)GetProcAddress(sl_.mod, n)
    GET(init, "slInit");
    GET(shutdown, "slShutdown");
    GET(supported, "slIsFeatureSupported");
    GET(tag, "slSetTagForFrame");
    GET(constants, "slSetConstants");
    GET(feature, "slGetFeatureFunction");
    GET(token, "slGetNewFrameToken");
    GET(gipa, "vkGetInstanceProcAddr");
    GET(gdpa, "vkGetDeviceProcAddr");
#undef GET
    if (!sl_.init || !sl_.feature || !sl_.token || !sl_.tag || !sl_.constants || !sl_.gipa || !sl_.gdpa) {
        SLOG("[streamline] entry points missing\n");
        FreeLibrary(sl_.mod);
        sl_.mod = NULL;
        return false;
    }
    static sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    static const wchar_t *plugins[1];
    static wchar_t plugdir[1024];
    wcscpy(plugdir, dir);
    plugins[0] = plugdir;
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = getenv("ODYSSEY_SL_LOG") ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;  // (ODYSSEY_SL_LOG=1)
    pref.logMessageCallback = sl_log;
    pref.pathsToPlugins = plugins;
    pref.numPathsToPlugins = 1;
    // (manual hooking, our own tags per frame; no over-the-air plugin downloads)
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    static sl::Feature only[3];
    uint32_t nf = 3;
    if (const char *fl = getenv("ODYSSEY_SL_FEATURES")) {  // (tests: "reflex", "dlssg")
        nf = 0;
        if (strstr(fl, "dlssg")) only[nf++] = sl::kFeatureDLSS_G;
        if (strstr(fl, "reflex")) only[nf++] = sl::kFeatureReflex;
        only[nf++] = sl::kFeaturePCL;
    }
    pref.featuresToLoad = getenv("ODYSSEY_SL_FEATURES") ? only : features;
    pref.numFeaturesToLoad = nf;
    pref.applicationId = 231313132;  // (the NGX development id DLSS uses here: dlss_probe.c)
    pref.engine = sl::EngineType::eCustom;
    pref.renderAPI = sl::RenderAPI::eVulkan;
    sl::Result r = sl_.init(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        SLOG("[streamline] slInit failed: %d\n", (int)r);
        FreeLibrary(sl_.mod);
        sl_.mod = NULL;
        return false;
    }
    sl_.active = true;
    sl_.dlssg_on = -1;
    SLOG("[streamline] initialised (manual hooking)\n");
    return true;
}

extern "C" bool vk_sl_active(void) { return sl_.active; }
extern "C" PFN_vkVoidFunction vk_sl_instance_proc(VkInstance inst, const char *name) {
    return sl_.active ? sl_.gipa(inst, name) : NULL;
}
extern "C" PFN_vkVoidFunction vk_sl_device_proc(VkDevice dev, const char *name) {
    return sl_.active ? sl_.gdpa(dev, name) : NULL;
}

extern "C" void vk_sl_device_ready(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t qfam) {
    (void)inst, (void)dev, (void)qfam;
    if (!sl_.active) return;
    sl::AdapterInfo ai{};
    ai.vkPhysicalDevice = phys;
    sl_.dlssg = sl_.supported && sl_.supported(sl::kFeatureDLSS_G, ai) == sl::Result::eOk;
    sl_.reflex = sl_.supported && sl_.supported(sl::kFeatureReflex, ai) == sl::Result::eOk;
    SLOG("[streamline] DLSS Frame Generation %s, Reflex %s\n", sl_.dlssg ? "available" : "unavailable",
         sl_.reflex ? "available" : "unavailable");
    if (sl_.reflex) {
        sl::ReflexOptions o{};
        // (DLSS-G needs Reflex on; Boost when asked)
        o.mode = g_settings.low_latency == 2 ? sl::ReflexMode::eLowLatencyWithBoost
               : g_settings.low_latency == 1 || (sl_.dlssg && g_settings.frame_gen == FG_DLSS) ? sl::ReflexMode::eLowLatency
               : sl::ReflexMode::eOff;
        sl::Result r = slReflexSetOptions(o);
        SLOG("[streamline] Reflex %s (%d)\n", o.mode == sl::ReflexMode::eOff ? "off" : o.mode == sl::ReflexMode::eLowLatency ?
             "on" : "on + boost", (int)r);
    }
}
extern "C" bool vk_sl_dlssg_available(void) { return sl_.active && sl_.dlssg; }
extern "C" bool vk_sl_reflex_available(void) { return sl_.active && sl_.reflex; }

static sl::FrameToken *token_for(uint64_t frame) {
    if (!sl_.cur || sl_.token_frame != frame) {
        uint32_t idx = (uint32_t)frame;
        sl::FrameToken *t = nullptr;
        if (sl_.token(t, &idx) == sl::Result::eOk && t) sl_.cur = t, sl_.token_frame = frame;
    }
    return sl_.cur;
}

extern "C" void vk_sl_marker(uint64_t frame, int marker) {
    if (!sl_.active || !sl_.reflex) return;
    sl::FrameToken *t = token_for(frame);
    if (t) slPCLSetMarker((sl::PCLMarker)marker, *t);
}

extern "C" void vk_sl_frame_start(uint64_t frame) {
    if (!sl_.active || !sl_.reflex) return;
    sl::FrameToken *t = token_for(frame);
    if (!t) return;
    slReflexSleep(*t);
    slPCLSetMarker(sl::PCLMarker::eSimulationStart, *t);
    slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *t);
    slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *t);
}

static void inverse4(const float *m, float *o) {  // (general 4x4; o unchanged if singular)
    double inv[16], a[16];
    for (int i = 0; i < 16; i++) a[i] = m[i];
    inv[0] = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    inv[4] = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    inv[8] = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    inv[1] = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    inv[5] = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    inv[9] = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    inv[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    inv[2] = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
    inv[6] = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
    inv[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
    inv[3] = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
    inv[7] = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11] - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
    inv[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10] + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];
    double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (fabs(det) < 1e-20) return;
    for (int i = 0; i < 16; i++) o[i] = (float)(inv[i] / det);
}

static void to_sl(sl::float4x4 &d, const float *m) {
    for (int r = 0; r < 4; r++) d.row[r] = sl::float4(m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
}

static sl::Resource resource(const SlImage &im) {
    sl::Resource r(sl::ResourceType::eTex2d, (void *)im.image, nullptr, (void *)im.view, (uint32_t)VK_IMAGE_LAYOUT_GENERAL);
    r.width = im.w, r.height = im.h, r.nativeFormat = (uint32_t)im.format, r.mipLevels = 1, r.arrayLayers = 1;
    r.flags = 0, r.usage = im.usage;
    return r;
}

extern "C" void vk_sl_frame(uint64_t frame, VkCommandBuffer cb, const SlFrame *f) {
    if (!sl_.active || !sl_.dlssg || g_settings.frame_gen != FG_DLSS) return;
    sl::FrameToken *t = token_for(frame);
    if (!t) return;
    sl::ViewportHandle vp(0u);
    sl::Constants c{};
    float clip_to_view[16], prev_to_clip[16];
    memset(clip_to_view, 0, sizeof clip_to_view), memset(prev_to_clip, 0, sizeof prev_to_clip);
    inverse4(f->view_to_clip, clip_to_view);
    inverse4(f->clip_to_prev_clip, prev_to_clip);
    to_sl(c.cameraViewToClip, f->view_to_clip);
    to_sl(c.clipToCameraView, clip_to_view);
    static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    to_sl(c.clipToLensClip, id);
    to_sl(c.clipToPrevClip, f->clip_to_prev_clip);
    to_sl(c.prevClipToClip, prev_to_clip);
    c.jitterOffset = sl::float2(f->jitter[0], f->jitter[1]);
    c.mvecScale = sl::float2(1.0f / f->mvec.w, 1.0f / f->mvec.h);  // (pixel motion vectors)
    c.cameraPinholeOffset = sl::float2(0, 0);
    c.cameraPos = sl::float3(0, 0, 0), c.cameraUp = sl::float3(0, 1, 0), c.cameraRight = sl::float3(1, 0, 0), c.cameraFwd = sl::float3(0, 0, 1);
    c.cameraNear = f->near_z, c.cameraFar = f->far_z, c.cameraFOV = f->fov_y, c.cameraAspectRatio = f->aspect;
    c.depthInverted = sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = f->reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    sl_.constants(c, *t, vp);
    sl::Resource depth = resource(f->depth), mvec = resource(f->mvec);
    sl::Extent de{0, 0, f->depth.w, f->depth.h}, me{0, 0, f->mvec.w, f->mvec.h};
    sl::ResourceTag tags[2] = {sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &de),
                               sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &me)};
    sl::CommandBuffer *cmd = (sl::CommandBuffer *)cb;
    sl::Result r = sl_.tag(*t, vp, tags, 2, cmd);
    static int nlog;
    if (r != sl::Result::eOk && nlog++ < 5) SLOG("[streamline] tagging failed: %d\n", (int)r);
    sl_.scene_frame = frame;
    if (getenv("ODYSSEY_SL_TRACE")) SLOG("[streamline] tags for frame %llu\n", (unsigned long long)frame);
    sl_.mw = f->mvec.w, sl_.mh = f->mvec.h;
}

extern "C" void vk_sl_before_present(uint64_t frame, uint32_t w, uint32_t h) {
    if (!sl_.active) return;
    sl::FrameToken *t = token_for(frame);
    if (getenv("ODYSSEY_SL_TRACE")) SLOG("[streamline] present frame %llu\n", (unsigned long long)frame);
    if (sl_.reflex && t) {
        slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *t);
        slPCLSetMarker(sl::PCLMarker::ePresentStart, *t);
    }
    if (!sl_.dlssg) return;
    {   // ([streamline] every 600 frames: what DLSS-G reports - frames presented for each one rendered)
        static uint64_t n, shown;
        sl::DLSSGState st{};
        sl::Result r = slDLSSGGetState(sl::ViewportHandle(0u), st, nullptr);  // (presented since the last call)
        shown += st.numFramesActuallyPresented;
        if (++n % 600 == 0) {
            SLOG("[streamline] DLSS-G: %llu frames presented for the last 600 rendered (status %u, max generated %u, "
                 "min size %u, vsync %d, %d)\n", (unsigned long long)shown, (unsigned)st.status, st.numFramesToGenerateMax,
                 st.minWidthOrHeight, (int)st.bIsVsyncSupportAvailable, (int)r);
            shown = 0;
        }
    }
    // (generation on for frames with a 3D scene tagged - menus, loading: shown as they are)
    // (on until 30 frames go by without one: a frame here or there without the scene found doesn't tear it down)
    int on = g_settings.frame_gen == FG_DLSS && sl_.scene_frame && frame - sl_.scene_frame < 30;
    if (on == sl_.dlssg_on && w == sl_.dw && h == sl_.dh) return;
    sl::DLSSGOptions o{};
    o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    o.numFramesToGenerate = 1;
    o.colorWidth = w, o.colorHeight = h;
    o.mvecDepthWidth = sl_.mw, o.mvecDepthHeight = sl_.mh;
    o.numBackBuffers = sl_.backbuffers;
    o.colorBufferFormat = (uint32_t)sl_.color_format;
    o.mvecBufferFormat = (uint32_t)VK_FORMAT_R16G16_SFLOAT;
    o.depthBufferFormat = (uint32_t)VK_FORMAT_D32_SFLOAT;
    sl::Result r = slDLSSGSetOptions(sl::ViewportHandle(0u), o);
    static int nlog;
    if (nlog++ < 8) SLOG("[streamline] DLSS Frame Generation %s (%ux%u, %d)\n", on ? "on" : "off", w, h, (int)r);
    sl_.dlssg_on = on, sl_.dw = w, sl_.dh = h;
}

extern "C" void vk_sl_shutdown(void) {
    if (!sl_.active) return;
    if (sl_.shutdown) sl_.shutdown();
    sl_.active = false;
}
#endif
