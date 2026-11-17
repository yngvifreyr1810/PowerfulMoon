// AMD FSR 2.2 temporal upscaler on our renderer: AMD's GLSL passes (third_party/fsr2/shaders, MIT, FSR 2.2.1), with
// the host side of AMD's ffx_fsr2.cpp redone here - its resources, constants and the six dispatches of a frame:
// luminance pyramid (SPD) -> reconstruct & dilate -> depth clip -> lock -> accumulate (-> RCAS sharpening).
// Shaders: read from fsr2/ next to the executable (in the repository: third_party/fsr2/shaders), their #includes
// expanded here, compiled with shaderc at the first use. Each pass's descriptors come from the bindings it declares
// (#define FSR2_BIND_<SRV|UAV|CB>_<NAME> <n>; set 0: the two samplers). Every image stays in the GENERAL layout,
// with a full barrier between passes.
// Permutation: full precision (no FP16), motion vectors at the render size and without the jitter, depth not
// inverted, LDR color, auto exposure on.
#include "fsr2.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../paths.h"
#include "../../platform.h"
#include "../../../third_party/fsr2/ffx_fsr2_maximum_bias.h"

enum {  // our resources
    R_COLOR, R_DEPTH, R_MOTION, R_OUTPUT,  // (the caller's)
    R_PREPARED, R_RECON, R_DMV1, R_DMV2, R_DDEPTH, R_LOCK1, R_LOCK2, R_LOCKLUMA, R_NEWLOCKS, R_UP1, R_UP2, R_SCENELUM,
    R_LH1, R_LH2, R_ATOMIC, R_DREACTIVE, R_LANCZOS, R_REACTIVE0, R_MAXBIAS, R_AUTOEXP,
    R_COUNT
};
enum { CB_FSR2, CB_SPD, CB_RCAS, CB_COUNT };
enum { P_PYRAMID, P_RECON, P_CLIP, P_LOCK, P_ACCUM, P_ACCUM_SHARP, P_RCAS, P_COUNT };
enum { K_SRV, K_UAV, K_CB };
// (a binding the pass declares: its kind, and what goes there - a role resolved each frame: see resolve())
enum { X_FIXED, X_LOCK_SRV, X_LOCK_UAV, X_UP_SRV, X_UP_UAV, X_DMV_CUR, X_DMV_PREV, X_LH_SRV, X_LH_UAV, X_MIP4, X_MIP5 };

typedef struct {
    VkImage image;
    VmaAllocation mem;
    VkImageView view, mip[2];  // (view: every mip, for reading; mip: levels 4 and 5 for writing - the scene luminance)
    VkFormat format;
    uint32_t w, h, mips;
} Img;

typedef struct { uint8_t binding, kind, role, res; } Bind;
typedef struct {
    VkDescriptorSetLayout set1;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    Bind b[24];
    int n;
} Pass;

static struct {
    int state;  // 0 untried, 1 ready, -1 failed (shaders missing or not built)
    VkSampler point, linear;
    VkDescriptorSetLayout set0;
    Pass pass[P_COUNT];
    Img img[R_COUNT];
    uint32_t mw, mh, dw, dh;  // the images' max render and display sizes
    uint32_t frame;           // resource frame index (its parity picks the ping-pong images)
    bool first;
    struct {  // = cbFSR2 (std140, the same offsets)
        int32_t renderSize[2], maxRenderSize[2], displaySize[2], inputColorResourceDimensions[2], lumaMipDimensions[2];
        int32_t lumaMipLevelToUse, frameIndex;
        float deviceToViewDepth[4], jitterOffset[2], motionVectorScale[2], downscaleFactor[2], motionVectorJitterCancellation[2];
        float preExposure, previousFramePreExposure, tanHalfFOV, jitterPhaseCount, deltaTime, dynamicResChangeFactor;
        float viewSpaceToMetersFactor;
    } c;
} f2;

// ------------------------------------------------------------------ shaders
static char *read_text(const char *name) {
    char rel[96], dev[160], path[1100];
    snprintf(rel, sizeof rel, "fsr2/%s", name);
    snprintf(dev, sizeof dev, "third_party/fsr2/shaders/%s", name);
    if (!rt_resource(rel, dev, path, sizeof path)) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

typedef struct { char *s; size_t n, cap; } Buf;
static void put(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

// The source with its #include "x" lines replaced by the files
static bool expand(Buf *out, const char *name, char done[][64], int *ndone, int depth) {
    (void)done, (void)ndone;  // (every #include expanded where it is: the headers' own guards keep them once)
    char *src = read_text(name);
    // (not shipped: the CPU / HLSL headers, included only under #if FFX_CPU / FFX_HLSL - the expansion here doesn't
    // evaluate #if; were one needed, the compile would say so)
    if (!src) return depth > 0;
    bool ok = true;
    for (char *p = src; *p && ok;) {
        char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p + 1) : strlen(p);
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        char inc[64];
        if (depth < 8 && sscanf(q, "#include \"%63[^\"]\"", inc) == 1) ok = expand(out, inc, done, ndone, depth + 1);
        else if (!strncmp(q, "#extension GL_GOOGLE_include_directive", 38)) {}  // (expanded here)
        else put(out, p, n);
        p += n;
    }
    put(out, "\n", 1);  // (a file without a last newline: its #endif not run into the next file's first line)
    free(src);
    return ok;
}

// Which resource a binding name means (FSR2_BIND_<KIND>_<NAME>); false: none of ours (declared but unused names)
static bool binding_target(int kind, const char *name, uint8_t *role, uint8_t *res) {
    static const struct { const char *n; uint8_t role, res; } srv[] = {
        {"INPUT_COLOR", X_FIXED, R_COLOR}, {"INPUT_MOTION_VECTORS", X_FIXED, R_MOTION}, {"INPUT_DEPTH", X_FIXED, R_DEPTH},
        {"INPUT_EXPOSURE", X_FIXED, R_AUTOEXP}, {"AUTO_EXPOSURE", X_FIXED, R_AUTOEXP},
        {"REACTIVE_MASK", X_FIXED, R_REACTIVE0}, {"TRANSPARENCY_AND_COMPOSITION_MASK", X_FIXED, R_REACTIVE0},
        {"RECONSTRUCTED_PREV_NEAREST_DEPTH", X_FIXED, R_RECON}, {"DILATED_MOTION_VECTORS", X_DMV_CUR, 0},
        {"PREVIOUS_DILATED_MOTION_VECTORS", X_DMV_PREV, 0}, {"DILATED_DEPTH", X_FIXED, R_DDEPTH},
        {"INTERNAL_UPSCALED", X_UP_SRV, 0}, {"LOCK_STATUS", X_LOCK_SRV, 0}, {"PREPARED_INPUT_COLOR", X_FIXED, R_PREPARED},
        {"LUMA_HISTORY", X_LH_SRV, 0}, {"RCAS_INPUT", X_UP_UAV, 0}, {"LANCZOS_LUT", X_FIXED, R_LANCZOS},
        {"SCENE_LUMINANCE_MIPS", X_FIXED, R_SCENELUM}, {"UPSCALE_MAXIMUM_BIAS_LUT", X_FIXED, R_MAXBIAS},
        {"DILATED_REACTIVE_MASKS", X_FIXED, R_DREACTIVE}, {"LOCK_INPUT_LUMA", X_FIXED, R_LOCKLUMA},
        {"NEW_LOCKS", X_FIXED, R_NEWLOCKS}};
    static const struct { const char *n; uint8_t role, res; } uav[] = {
        {"SPD_GLOBAL_ATOMIC", X_FIXED, R_ATOMIC}, {"EXPOSURE_MIP_LUMA_CHANGE", X_MIP4, R_SCENELUM},
        {"EXPOSURE_MIP_5", X_MIP5, R_SCENELUM}, {"AUTO_EXPOSURE", X_FIXED, R_AUTOEXP},
        {"RECONSTRUCTED_PREV_NEAREST_DEPTH", X_FIXED, R_RECON}, {"DILATED_MOTION_VECTORS", X_DMV_CUR, 0},
        {"DILATED_DEPTH", X_FIXED, R_DDEPTH}, {"PREPARED_INPUT_COLOR", X_FIXED, R_PREPARED}, {"LUMA_HISTORY", X_LH_UAV, 0},
        {"LOCK_INPUT_LUMA", X_FIXED, R_LOCKLUMA}, {"DILATED_REACTIVE_MASKS", X_FIXED, R_DREACTIVE},
        {"NEW_LOCKS", X_FIXED, R_NEWLOCKS}, {"INTERNAL_UPSCALED", X_UP_UAV, 0}, {"LOCK_STATUS", X_LOCK_UAV, 0},
        {"UPSCALED_OUTPUT", X_FIXED, R_OUTPUT}};
    if (kind == K_CB) {
        *role = X_FIXED;
        *res = !strcmp(name, "FSR2") ? CB_FSR2 : !strcmp(name, "SPD") ? CB_SPD : !strcmp(name, "RCAS") ? CB_RCAS : 255;
        return *res != 255;
    }
    int n = kind == K_SRV ? (int)(sizeof srv / sizeof *srv) : (int)(sizeof uav / sizeof *uav);
    for (int i = 0; i < n; i++) {
        const char *m = kind == K_SRV ? srv[i].n : uav[i].n;
        if (!strcmp(m, name)) {
            *role = kind == K_SRV ? srv[i].role : uav[i].role;
            *res = kind == K_SRV ? srv[i].res : uav[i].res;
            return true;
        }
    }
    return false;
}

static bool build_pass(Pass *p, const char *file, const char *options) {
    Buf b = {0};
    char done[48][64];
    int ndone = 0;
    if (!expand(&b, file, done, &ndone, 0)) { free(b.s); return false; }
    // the options after #version (which must come first)
    char *v = strstr(b.s, "#version");
    char *eol = v ? strchr(v, '\n') : NULL;
    if (!eol) { free(b.s); return false; }
    Buf src = {0};
    put(&src, b.s, (size_t)(eol - b.s + 1));
    put(&src, options, strlen(options));
    put(&src, eol + 1, strlen(eol + 1));
    free(b.s);
    // the bindings it declares: "#define FSR2_BIND_<KIND>_<NAME> <n>" with a "binding = FSR2_BIND_<KIND>_<NAME>" in a
    // layout that is compiled in (one number given to two names under #if: the one our options keep - low resolution
    // motion vectors: the dilated ones)
    p->n = 0;
    VkDescriptorSetLayoutBinding lb[24];
    for (char *q = src.s; (q = strstr(q, "#define FSR2_BIND_")) != NULL; q++) {
        char kind[4] = "", name[64] = "";
        int num = -1;
        if (sscanf(q + 18, "%3[A-Z]_%63[A-Z0-9_] %d", kind, name, &num) != 3 || num < 0 || num > 31) continue;
        int k = !strcmp(kind, "SRV") ? K_SRV : !strcmp(kind, "UAV") ? K_UAV : !strcmp(kind, "CB") ? K_CB : -1;
        if (k < 0) continue;
        char use[96];
        snprintf(use, sizeof use, "binding = FSR2_BIND_%s_%s", kind, name);
        if (!strstr(src.s, use)) continue;  // (named, never declared)
        if (k == K_SRV && !strcmp(name, "INPUT_MOTION_VECTORS")) {
            bool dup = false;
            for (int i = 0; i < p->n; i++) dup |= p->b[i].binding == num;
            if (dup || strstr(src.s, "#define FSR2_BIND_SRV_DILATED_MOTION_VECTORS                 2")) continue;
        }
        bool seen = false;
        for (int i = 0; i < p->n; i++) seen |= p->b[i].binding == num;
        if (seen || p->n == 24) continue;
        Bind *bd = &p->b[p->n];
        bd->binding = (uint8_t)num, bd->kind = (uint8_t)k;
        if (!binding_target(k, name, &bd->role, &bd->res)) continue;
        lb[p->n] = (VkDescriptorSetLayoutBinding){(uint32_t)num, k == K_SRV ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE :
            k == K_UAV ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        p->n++;
    }
    VkShaderModule m = vk_compile_glsl(src.s, STAGE_COMPUTE, file);
    free(src.s);
    if (!m) return false;
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = (uint32_t)p->n, .pBindings = lb};
    VkDescriptorSetLayout sets[2] = {f2.set0, VK_NULL_HANDLE};
    VkResult r = vkCreateDescriptorSetLayout(g_vk.dev, &si, NULL, &p->set1);
    sets[1] = p->set1;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2, .pSetLayouts = sets};
    if (r == VK_SUCCESS) r = vkCreatePipelineLayout(g_vk.dev, &li, NULL, &p->layout);
    VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = m, .pName = "main"}, .layout = p->layout};
    if (r == VK_SUCCESS) r = vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, &p->pipeline);
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    if (r != VK_SUCCESS) { LOG("[fsr2] %s: pipeline failed (%d)\n", file, r); return false; }
    return true;
}

static bool init_pipelines(void) {
    VkSamplerCreateInfo sc = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = VK_LOD_CLAMP_NONE};
    if (vkCreateSampler(g_vk.dev, &sc, NULL, &f2.point) != VK_SUCCESS) return false;
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR, sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (vkCreateSampler(g_vk.dev, &sc, NULL, &f2.linear) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding sb[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                          {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = sb};
    if (vkCreateDescriptorSetLayout(g_vk.dev, &si, NULL, &f2.set0) != VK_SUCCESS) return false;
    static const char base[] =
        "#define FFX_GPU 1\n#define FFX_GLSL 1\n#define FFX_HALF 0\n"
        "#define FFX_FSR2_OPTION_HDR_COLOR_INPUT 0\n#define FFX_FSR2_OPTION_LOW_RESOLUTION_MOTION_VECTORS 1\n"
        "#define FFX_FSR2_OPTION_JITTERED_MOTION_VECTORS 0\n#define FFX_FSR2_OPTION_INVERTED_DEPTH 0\n"
        "#define FFX_FSR2_OPTION_REPROJECT_USE_LANCZOS_TYPE 0\n#define FFX_FSR2_OPTION_UPSAMPLE_USE_LANCZOS_TYPE 0\n"
        "#define FFX_FSR2_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF 0\n#define FFX_FSR2_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF 0\n"
        "#define FFX_FSR2_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF 0\n";
    char sharp[sizeof base + 64];
    snprintf(sharp, sizeof sharp, "%s#define FFX_FSR2_OPTION_APPLY_SHARPENING 1\n", base);
    char plain[sizeof base + 64];
    snprintf(plain, sizeof plain, "%s#define FFX_FSR2_OPTION_APPLY_SHARPENING 0\n", base);
    static const char *const files[P_COUNT] = {"ffx_fsr2_compute_luminance_pyramid_pass.glsl",
        "ffx_fsr2_reconstruct_previous_depth_pass.glsl", "ffx_fsr2_depth_clip_pass.glsl", "ffx_fsr2_lock_pass.glsl",
        "ffx_fsr2_accumulate_pass.glsl", "ffx_fsr2_accumulate_pass.glsl", "ffx_fsr2_rcas_pass.glsl"};
    uint64_t t0 = plat_now_ns();
    for (int i = 0; i < P_COUNT; i++)
        if (!build_pass(&f2.pass[i], files[i], i == P_ACCUM_SHARP ? sharp : plain)) return false;
    LOG("[fsr2] passes built in %.0f ms\n", (plat_now_ns() - t0) / 1e6);
    return true;
}

// (any thread: true until a build failed - the build itself happens at the first dispatch, on the render thread)
bool vk_fsr2_available(void) { return f2.state >= 0 && g_vk.storage_write_without_format; }
static bool ensure_built(void) {
    if (f2.state == 0) {
        f2.state = init_pipelines() ? 1 : -1;
        if (f2.state < 0) LOG("[fsr2] unavailable: shaders missing or not built\n");
    }
    return f2.state > 0;
}

// ------------------------------------------------------------------ resources
static bool make(Img *im, VkCommandBuffer cb, uint32_t w, uint32_t h, uint32_t mips, VkFormat fmt, bool storage) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = {w, h, 1}, .mipLevels = mips, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0)};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &im->image, &im->mem, NULL) != VK_SUCCESS) { im->image = VK_NULL_HANDLE; return false; }
    im->format = fmt, im->w = w, im->h = h, im->mips = mips;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &im->view) != VK_SUCCESS) return false;
    for (int k = 0; k < 2 && mips > 5; k++) {
        vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 4u + (uint32_t)k, 1, 0, 1};
        if (vkCreateImageView(g_vk.dev, &vi, NULL, &im->mip[k]) != VK_SUCCESS) return false;
    }
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im->image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    return true;
}

static void destroy(Img *im) {
    for (int k = 0; k < 2; k++) if (im->mip[k]) vkDestroyImageView(g_vk.dev, im->mip[k], NULL);
    if (im->view) vkDestroyImageView(g_vk.dev, im->view, NULL);
    if (im->image) vmaDestroyImage(g_vk.vma, im->image, im->mem);
    *im = (Img){0};
}

void vk_fsr2_release(void) {
    for (int i = R_PREPARED; i < R_COUNT; i++) destroy(&f2.img[i]);
    f2.mw = f2.mh = f2.dw = f2.dh = 0;
}

static void clear(VkCommandBuffer cb, Img *im, float r, float g) {
    VkClearColorValue v = {.float32 = {r, g, 0, 0}};
    if (im->format == VK_FORMAT_R32_UINT) v = (VkClearColorValue){.uint32 = {(uint32_t)r, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, im->mips, 0, 1};
    vkCmdClearColorImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &all);
}

static void upload(VkCommandBuffer cb, Img *im, const void *data, uint32_t bytes) {
    VkSpan s = vk_transient_copy(data, bytes, 16);
    VkBufferImageCopy rg = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {im->w, im->h, 1}};
    vkCmdCopyBufferToImage(cb, s.buf, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
}

static float lanczos2(float x) {
    const float pi = 3.14159265f;
    return fabsf(x) < 1e-6f ? 1.f : (sinf(pi * x) / (pi * x)) * (sinf(0.5f * pi * x) / (0.5f * pi * x));
}

static bool create_resources(VkCommandBuffer cb, uint32_t mw, uint32_t mh, uint32_t dw, uint32_t dh) {
    vk_fsr2_release();
    struct { int id; uint32_t w, h, mips; VkFormat f; bool storage; } d[] = {
        // (prepared color: YCoCg, signed - float as AMD's own code makes it, though the shader names rgba16)
        {R_PREPARED, mw, mh, 1, VK_FORMAT_R16G16B16A16_SFLOAT, true}, {R_RECON, mw, mh, 1, VK_FORMAT_R32_UINT, true},
        {R_DMV1, mw, mh, 1, VK_FORMAT_R16G16_SFLOAT, true}, {R_DMV2, mw, mh, 1, VK_FORMAT_R16G16_SFLOAT, true},
        {R_DDEPTH, mw, mh, 1, VK_FORMAT_R16_SFLOAT, true}, {R_LOCK1, dw, dh, 1, VK_FORMAT_R16G16_SFLOAT, true},
        {R_LOCK2, dw, dh, 1, VK_FORMAT_R16G16_SFLOAT, true}, {R_LOCKLUMA, mw, mh, 1, VK_FORMAT_R16_SFLOAT, true},
        {R_NEWLOCKS, dw, dh, 1, VK_FORMAT_R8_UNORM, true}, {R_UP1, dw, dh, 1, VK_FORMAT_R16G16B16A16_SFLOAT, true},
        {R_UP2, dw, dh, 1, VK_FORMAT_R16G16B16A16_SFLOAT, true}, {R_SCENELUM, mw / 2, mh / 2, 0, VK_FORMAT_R16_SFLOAT, true},
        {R_LH1, dw, dh, 1, VK_FORMAT_R8G8B8A8_UNORM, true}, {R_LH2, dw, dh, 1, VK_FORMAT_R8G8B8A8_UNORM, true},
        {R_ATOMIC, 1, 1, 1, VK_FORMAT_R32_UINT, true}, {R_DREACTIVE, mw, mh, 1, VK_FORMAT_R8G8_UNORM, true},
        {R_LANCZOS, 128, 1, 1, VK_FORMAT_R16_SNORM, false}, {R_REACTIVE0, 1, 1, 1, VK_FORMAT_R8_UNORM, false},
        {R_MAXBIAS, FFX_FSR2_MAXIMUM_BIAS_TEXTURE_WIDTH, FFX_FSR2_MAXIMUM_BIAS_TEXTURE_HEIGHT, 1, VK_FORMAT_R16_SNORM, false},
        {R_AUTOEXP, 1, 1, 1, VK_FORMAT_R32G32_SFLOAT, true}};
    for (size_t i = 0; i < sizeof d / sizeof *d; i++) {
        uint32_t mips = d[i].mips;
        if (!mips) { uint32_t m = d[i].w > d[i].h ? d[i].w : d[i].h; mips = 1; while (m > 1) m >>= 1, mips++; }
        if (!make(&f2.img[d[i].id], cb, d[i].w, d[i].h, mips, d[i].f, d[i].storage)) {
            LOG("[fsr2] image %d (%ux%u format %d) failed\n", d[i].id, d[i].w, d[i].h, d[i].f);
            vk_fsr2_release();
            return false;
        }
    }
    int16_t lut[128], bias[FFX_FSR2_MAXIMUM_BIAS_TEXTURE_WIDTH * FFX_FSR2_MAXIMUM_BIAS_TEXTURE_HEIGHT];
    for (int i = 0; i < 128; i++) lut[i] = (int16_t)lroundf(lanczos2(2.0f * i / 127) * 32767.0f);
    for (int i = 0; i < FFX_FSR2_MAXIMUM_BIAS_TEXTURE_WIDTH * FFX_FSR2_MAXIMUM_BIAS_TEXTURE_HEIGHT; i++)
        bias[i] = (int16_t)lroundf(ffxFsr2MaximumBias[i] / 2.0f * 32767.0f);
    upload(cb, &f2.img[R_LANCZOS], lut, sizeof lut);
    upload(cb, &f2.img[R_MAXBIAS], bias, sizeof bias);
    clear(cb, &f2.img[R_REACTIVE0], 0, 0);
    clear(cb, &f2.img[R_ATOMIC], 0, 0);
    f2.mw = mw, f2.mh = mh, f2.dw = dw, f2.dh = dh;
    f2.first = true;
    memset(&f2.c, 0, sizeof f2.c);
    LOG("[fsr2] images: render %ux%u -> display %ux%u\n", mw, mh, dw, dh);
    return true;
}

// ------------------------------------------------------------------ a frame
static VkImageView view_of(const Fsr2Frame *f, const Bind *b) {
    bool odd = f2.frame & 1;
    int r = b->res;
    switch (b->role) {
    case X_LOCK_SRV: r = odd ? R_LOCK2 : R_LOCK1; break;
    case X_LOCK_UAV: r = odd ? R_LOCK1 : R_LOCK2; break;
    case X_UP_SRV: r = odd ? R_UP2 : R_UP1; break;
    case X_UP_UAV: r = odd ? R_UP1 : R_UP2; break;
    case X_DMV_CUR: r = odd ? R_DMV2 : R_DMV1; break;
    case X_DMV_PREV: r = odd ? R_DMV1 : R_DMV2; break;
    case X_LH_SRV: r = odd ? R_LH2 : R_LH1; break;
    case X_LH_UAV: r = odd ? R_LH1 : R_LH2; break;
    case X_MIP4: return f2.img[R_SCENELUM].mip[0];
    case X_MIP5: return f2.img[R_SCENELUM].mip[1];
    }
    if (r == R_COLOR) return f->color_view;
    if (r == R_DEPTH) return f->depth_view;
    if (r == R_MOTION) return f->motion_view;
    if (r == R_OUTPUT) return f->output_view;
    return f2.img[r].view;
}

static void dispatch(VkCommandBuffer cb, const Fsr2Frame *f, int pi, const VkSpan *cbs, const uint32_t *cbsize,
                     uint32_t gx, uint32_t gy) {
    Pass *p = &f2.pass[pi];
    VkDescriptorSet s0 = vk_alloc_set(f2.set0), s1 = vk_alloc_set(p->set1);
    VkDescriptorImageInfo smp[2] = {{f2.point, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}, {f2.linear, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}};
    VkWriteDescriptorSet w[26];
    VkDescriptorImageInfo ii[24];
    VkDescriptorBufferInfo bi[24];
    int n = 0;
    for (int k = 0; k < 2; k++)
        w[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s0, .dstBinding = (uint32_t)k,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &smp[k]};
    for (int i = 0; i < p->n; i++) {
        const Bind *b = &p->b[i];
        VkWriteDescriptorSet *x = &w[n++];
        *x = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s1, .dstBinding = b->binding,
                                    .descriptorCount = 1};
        if (b->kind == K_CB) {
            bi[i] = (VkDescriptorBufferInfo){cbs[b->res].buf, cbs[b->res].off, cbsize[b->res]};
            x->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, x->pBufferInfo = &bi[i];
        } else {
            ii[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view_of(f, b), VK_IMAGE_LAYOUT_GENERAL};
            x->descriptorType = b->kind == K_SRV ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            x->pImageInfo = &ii[i];
        }
    }
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    VkDescriptorSet sets[2] = {s0, s1};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 2, sets, 0, NULL);
    vkCmdDispatch(cb, gx, gy, 1);
    vk_full_barrier(cb);
}

static uint16_t half_of(float v) {  // (RCAS's packed half: round to nearest, normal range only)
    union { float f; uint32_t u; } x = {v};
    uint32_t s = (x.u >> 16) & 0x8000, e = (x.u >> 23) & 0xff, m = x.u & 0x7fffff;
    if (e < 113) return (uint16_t)s;
    if (e > 142) return (uint16_t)(s | 0x7c00);
    return (uint16_t)(s | ((e - 112) << 10) | ((m + 0x1000) >> 13));
}

bool vk_fsr2_dispatch(VkCommandBuffer cb, const Fsr2Frame *f) {
    if (!vk_fsr2_available() || !ensure_built()) return false;
    if (!f2.img[R_PREPARED].image || f->w > f2.mw || f->h > f2.mh || f->ow != f2.dw || f->oh != f2.dh)
        if (!create_resources(cb, f->w, f->h, f->ow, f->oh)) return false;
    bool odd = f2.frame & 1;
    bool reset = f->reset || f2.first;
    if (f2.first) {
        clear(cb, &f2.img[R_LOCK1], 0, 0);
        clear(cb, &f2.img[R_LOCK2], 0, 0);
        clear(cb, &f2.img[R_PREPARED], 0, 0);
    }
    f2.first = false;
    // constants (as ffx_fsr2.cpp fsr2Dispatch)
    f2.c.jitterOffset[0] = f->jitter[0], f2.c.jitterOffset[1] = f->jitter[1];
    f2.c.renderSize[0] = (int32_t)f->w, f2.c.renderSize[1] = (int32_t)f->h;
    f2.c.maxRenderSize[0] = (int32_t)f2.mw, f2.c.maxRenderSize[1] = (int32_t)f2.mh;
    f2.c.displaySize[0] = (int32_t)f->ow, f2.c.displaySize[1] = (int32_t)f->oh;
    f2.c.inputColorResourceDimensions[0] = (int32_t)f->w, f2.c.inputColorResourceDimensions[1] = (int32_t)f->h;
    float aspect = (float)f->w / f->h;
    f2.c.tanHalfFOV = tanf(atanf(tanf(f->fov_y / 2) * aspect));
    f2.c.viewSpaceToMetersFactor = 0.01f;
    {   // device depth -> view depth (not inverted, not infinite)
        float lo = fminf(f->near_z, f->far_z), hi = fmaxf(f->near_z, f->far_z), q = hi / (lo - hi);
        float cot = cosf(0.5f * f->fov_y) / sinf(0.5f * f->fov_y);
        f2.c.deviceToViewDepth[0] = -q, f2.c.deviceToViewDepth[1] = q * lo;
        f2.c.deviceToViewDepth[2] = aspect / cot, f2.c.deviceToViewDepth[3] = 1 / cot;
    }
    f2.c.downscaleFactor[0] = (float)f->w / f->ow, f2.c.downscaleFactor[1] = (float)f->h / f->oh;
    f2.c.previousFramePreExposure = f2.c.preExposure ? f2.c.preExposure : 1;
    f2.c.preExposure = 1;
    f2.c.motionVectorScale[0] = 1.0f / f->w, f2.c.motionVectorScale[1] = 1.0f / f->h;  // (pixel motion vectors)
    f2.c.motionVectorJitterCancellation[0] = f2.c.motionVectorJitterCancellation[1] = 0;
    float phases = floorf(8.0f * powf((float)f->ow / f->w, 2.0f));
    if (reset || f2.c.jitterPhaseCount == 0) f2.c.jitterPhaseCount = phases;
    else if (phases > f2.c.jitterPhaseCount) f2.c.jitterPhaseCount++;
    else if (phases < f2.c.jitterPhaseCount) f2.c.jitterPhaseCount--;
    f2.c.deltaTime = fmaxf(0, fminf(1, f->dt_ms / 1000.0f));
    f2.c.frameIndex = reset ? 0 : f2.c.frameIndex + 1;
    f2.c.lumaMipLevelToUse = 4;  // (FFX_FSR2_SHADING_CHANGE_MIP_LEVEL)
    f2.c.lumaMipDimensions[0] = (int32_t)(f2.mw / 32), f2.c.lumaMipDimensions[1] = (int32_t)(f2.mh / 32);
    f2.c.dynamicResChangeFactor = 0;
    if (reset) {
        clear(cb, &f2.img[odd ? R_LOCK2 : R_LOCK1], 0, 0);
        clear(cb, &f2.img[odd ? R_UP2 : R_UP1], 0, 0);
        clear(cb, &f2.img[R_SCENELUM], 0, 0);
        clear(cb, &f2.img[R_AUTOEXP], -1, 1e8f);
    }
    vk_full_barrier(cb);
    // SPD (luminance pyramid) setup: 64x64 tiles
    uint32_t tiles_x = (f->w - 1) / 64 + 1, tiles_y = (f->h - 1) / 64 + 1;
    uint32_t m = f->w > f->h ? f->w : f->h, mips = 0;
    while (m > 1) m >>= 1, mips++;
    if (mips > 12) mips = 12;
    uint32_t spd[6] = {mips, tiles_x * tiles_y, 0, 0, f->w, f->h};
    float s = exp2f(-((-2.0f * f->sharpness) + 2.0f));
    uint32_t rcas[4];
    memcpy(&rcas[0], &s, 4);
    rcas[1] = (uint32_t)half_of(s) | ((uint32_t)half_of(s) << 16), rcas[2] = rcas[3] = 0;
    uint64_t align = g_vk.props.limits.minUniformBufferOffsetAlignment;
    VkSpan cbs[CB_COUNT] = {vk_transient_copy(&f2.c, sizeof f2.c, align), vk_transient_copy(spd, sizeof spd, align),
                            vk_transient_copy(rcas, sizeof rcas, align)};
    uint32_t cbsize[CB_COUNT] = {sizeof f2.c, sizeof spd, sizeof rcas};
    uint32_t sx = (f->w + 7) / 8, sy = (f->h + 7) / 8, dx = (f->ow + 7) / 8, dy = (f->oh + 7) / 8;
    bool sharpen = f->sharpness > 0;
    dispatch(cb, f, P_PYRAMID, cbs, cbsize, tiles_x, tiles_y);
    dispatch(cb, f, P_RECON, cbs, cbsize, sx, sy);
    dispatch(cb, f, P_CLIP, cbs, cbsize, sx, sy);
    dispatch(cb, f, P_LOCK, cbs, cbsize, sx, sy);
    dispatch(cb, f, sharpen ? P_ACCUM_SHARP : P_ACCUM, cbs, cbsize, dx, dy);
    if (sharpen) dispatch(cb, f, P_RCAS, cbs, cbsize, (f->ow + 15) / 16, (f->oh + 15) / 16);
    f2.frame = (f2.frame + 1) % 16;
    return true;
}
