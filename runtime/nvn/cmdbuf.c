// NVN command buffers. Commands are recorded as CmdRec records (the call's arguments and the data it points to),
// which a queue turns into GPU work when it runs the list (runtime/nvn/vulkan/exec.c, renderer.m exec_list).
//
// Command memory: the game hands NVN memory sized for the real driver's compact GPU commands, and a CmdRec is several
// times bigger (Odyssey records its display lists into a fixed scratch buffer, which they overflowed). So the records
// live in host memory (an arena per chunk of command memory the game adds), and the game's memory is only counted:
// a record takes guest_cost() of it, about what the driver would use. The game's sizes, its Used / Free queries and
// its out-of-memory callbacks work as designed. A command handle is the guest address a list was begun at; cmd_list()
// finds its records. Memory added again ends the lists begun in it (NVN's rule), and an arena with none alive goes.
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "nvn.h"
#include "rqueue.h"

#define GUEST_RESERVE 16  // (kept free at the end of the game's memory, as the driver keeps room for its jump)

static uint32_t guest_cost(uint32_t extra) { return 8 + ((extra + 7) & ~7u); }

// A command buffer's records since it was last given memory. Freed once no list begun in it is alive and nothing
// records into it any more.
typedef struct ArenaBlock { struct ArenaBlock *next; size_t cap, used; uint8_t data[]; } ArenaBlock;
typedef struct Arena {
    ArenaBlock *blocks, *cur;  // (a block that fills up jumps to the next)
    int live;                  // lists begun in it that are still alive (g_list_lock)
    bool open;                 // a command buffer records into it
} Arena;

// Every alive list: its handle (the guest address it was begun at) -> its first record. Sorted by handle.
typedef struct { uint64_t handle; CmdRec *rec; Arena *arena; } ListStart;
static pthread_mutex_t g_list_lock = PTHREAD_MUTEX_INITIALIZER;
static ListStart *g_lists;
static int g_nlists, g_lists_cap;
static _Atomic uint64_t g_list_gen;  // changes with every list begun or ended (command buffers re-check their next_list)

static _Atomic uint64_t g_rec_bytes;  // (the status line: host memory of all records)
uint64_t nvn_record_bytes(void) { return atomic_load(&g_rec_bytes); }

static ArenaBlock *block_new(size_t cap) {
    ArenaBlock *b = malloc(sizeof *b + cap);
    if (!b) FATAL("nvn: out of host memory for commands\n");
    b->next = NULL, b->cap = cap, b->used = 0;
    atomic_fetch_add(&g_rec_bytes, cap);
    return b;
}

static void arena_free(Arena *a) {
    for (ArenaBlock *b = a->blocks, *n; b; b = n) n = b->next, atomic_fetch_sub(&g_rec_bytes, b->cap), free(b);
    free(a);
}
// With the render thread (rqueue.c) a list the game records over may still be queued or running there, or in a frame
// interpolation replay: its records go once the thread is past everything queued when it ended (the game overwrote a
// display list at a stage start while the thread ran it: an access violation in exec_list).
typedef struct { Arena *a; uint64_t ticket; } Grave;
static Grave *graves;
static int ngraves, cap_graves;
static void arena_release(Arena *a) {  // (g_list_lock)
    if (!a || a->live || a->open) return;
    if (!rq_enabled()) { arena_free(a); return; }
    if (ngraves == cap_graves) graves = realloc(graves, sizeof *graves * (size_t)(cap_graves = cap_graves ? cap_graves * 2 : 64));
    graves[ngraves++] = (Grave){a, rq_last_ticket()};
}
void cmd_reap(uint64_t done) {  // (the render thread, after an item: what no queued list or replay can reach any more)
    if (!ngraves) return;
    pthread_mutex_lock(&g_list_lock);
    int k = 0;
    for (int i = 0; i < ngraves; i++)
        if (graves[i].ticket <= done) arena_free(graves[i].a);
        else graves[k++] = graves[i];
    ngraves = k;
    pthread_mutex_unlock(&g_list_lock);
}

static int list_lower_bound(uint64_t handle) {  // (g_list_lock)
    int lo = 0, hi = g_nlists;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g_lists[mid].handle < handle) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// Room for a record of `size` bytes (each block keeps room for the jump to the next).
static CmdRec *arena_rec(Arena *a, uint32_t op, uint32_t size) {
    ArenaBlock *b = a->cur;
    if (b->used + size + sizeof(CmdRec) > b->cap) {
        size_t cap = b->cap * 2;
        if (cap < size + sizeof(CmdRec)) cap = size + sizeof(CmdRec);
        ArenaBlock *n = block_new(cap);
        CmdRec *j = (CmdRec *)(b->data + b->used);
        memset(j, 0, sizeof *j);
        j->op = CMD_JUMP, j->size = sizeof(CmdRec), j->x[0] = (uint64_t)(uintptr_t)n->data;
        b->used += sizeof(CmdRec);
        b->next = n;
        a->cur = b = n;
    }
    CmdRec *r = (CmdRec *)(b->data + b->used);
    memset(r, 0, sizeof *r);
    r->op = op, r->size = size;
    b->used += size;
    return r;
}

// Records about to count at [at, end) of cb's memory: lists begun there are overwritten now, they end. A list ends only
// then, not when its memory is merely given to a command buffer again: Odyssey hands out memory overlapping display
// lists it keeps calling (the new owner records less than would reach them), and they went missing - skinned meshes'
// vertex buffer binds in the depth pre-pass (garbage depth, holes in the shaded scene). next_list keeps it cheap.
static void lists_overwritten(NvnCmdBuf *cb, uint64_t at, uint64_t end) {
    if (cb->list_gen == atomic_load(&g_list_gen) && (!cb->next_list || cb->next_list >= end)) return;
    pthread_mutex_lock(&g_list_lock);
    int i = list_lower_bound(at), j = i;
    while (j < g_nlists && g_lists[j].handle < end) {
        Arena *la = g_lists[j++].arena;
        la->live--;
        arena_release(la);  // (an arena still recorded into stays)
    }
    if (j > i) {
        memmove(&g_lists[i], &g_lists[j], (g_nlists - j) * sizeof *g_lists);
        g_nlists -= j - i;
        atomic_fetch_add(&g_list_gen, 1);
    }
    cb->next_list = i < g_nlists && g_lists[i].handle < cb->mem + cb->size ? g_lists[i].handle : 0;
    cb->list_gen = atomic_load(&g_list_gen);
    pthread_mutex_unlock(&g_list_lock);
}

// The records of the list whose handle this is; NULL if no list was begun there (or its memory was given again).
CmdRec *cmd_list(uint64_t handle) {
    CmdRec *r = NULL;
    pthread_mutex_lock(&g_list_lock);
    int i = list_lower_bound(handle);
    if (i < g_nlists && g_lists[i].handle == handle) r = g_lists[i].rec;
    pthread_mutex_unlock(&g_list_lock);
    if (!r) {
        static int warned;
        if (warned++ < 10) LOG("[nvn] command handle %llx: no list recorded there (skipped)\n", (unsigned long long)handle);
    }
    return r;
}

static NvnCmdBuf *cb_of(uint64_t guest) { return HOST(NvnCmdBuf, guest); }

NVN(nvnCommandBufferInitialize) {
    NvnCmdBuf *cb = calloc(1, sizeof *cb);
    cb->guest = A(0);
    SET_HOST(A(0), cb);
    RET(1);
}
NVN(nvnCommandBufferFinalize) {
    NvnCmdBuf *cb = cb_of(A(0));
    if (!cb) return;
    pthread_mutex_lock(&g_list_lock);  // (its lists stay: they live as long as their memory)
    if (cb->arena) cb->arena->open = false, arena_release(cb->arena);
    pthread_mutex_unlock(&g_list_lock);
    free(cb);
    SET_HOST(A(0), 0);
}
NVN(nvnCommandBufferSetDebugLabel) { (void)c; }

NVN(nvnCommandBufferAddCommandMemory) {
    plat_heap_check("AddCommandMemory");
    NvnCmdBuf *cb = cb_of(A(0));
    uint64_t mem = HOST(NvnPool, A(1))->storage + A(2), size = A(3);
    Arena *a = calloc(1, sizeof *a), *old = cb->arena;
    a->blocks = a->cur = block_new(size * 8 > 1024 ? size * 8 : 1024);
    a->open = true;
    pthread_mutex_lock(&g_list_lock);
    // (lists begun in this memory end once records overwrite them: lists_overwritten)
    static int log_lists = -1;
    if (log_lists < 0) log_lists = getenv("ODYSSEY_LOG_LISTS") != NULL;
    if (log_lists) LOG("[lists] cb %llx memory %llx+%llx (was %llx+%llx, used %llx)\n", (unsigned long long)A(0), (unsigned long long)mem,
                       (unsigned long long)size, (unsigned long long)cb->mem, (unsigned long long)cb->size, (unsigned long long)cb->used);
    int i = list_lower_bound(mem);
    cb->next_list = i < g_nlists && g_lists[i].handle < mem + size ? g_lists[i].handle : 0;
    cb->list_gen = atomic_load(&g_list_gen);
    if (old) {
        if (cb->recording) {  // the list being recorded goes on in the new memory
            CmdRec *jr = arena_rec(old, CMD_JUMP, sizeof(CmdRec));
            jr->x[0] = (uint64_t)(uintptr_t)a->cur->data;
        }
        old->open = false;
        arena_release(old);
    }
    pthread_mutex_unlock(&g_list_lock);
    cb->arena = a;
    cb->mem = mem;
    cb->size = size;
    cb->used = 0;
}
NVN(nvnCommandBufferAddControlMemory) { (void)c; }
NVN(nvnCommandBufferGetCommandMemorySize) { RET(cb_of(A(0))->size); }
NVN(nvnCommandBufferGetCommandMemoryUsed) { RET(cb_of(A(0))->used); }
NVN(nvnCommandBufferGetCommandMemoryFree) {
    NvnCmdBuf *cb = cb_of(A(0));
    RET(cb->size > cb->used + GUEST_RESERVE ? cb->size - cb->used - GUEST_RESERVE : 0);
}
NVN(nvnCommandBufferGetControlMemorySize) { RET(0x10000); }
NVN(nvnCommandBufferGetControlMemoryUsed) { RET(0); }
NVN(nvnCommandBufferGetControlMemoryFree) { RET(0x10000); }
NVN(nvnCommandBufferSetMemoryCallback) { cb_of(A(0))->callback = A(1); }
NVN(nvnCommandBufferGetMemoryCallback) { RET(cb_of(A(0))->callback); }
NVN(nvnCommandBufferSetMemoryCallbackData) { cb_of(A(0))->callback_data = A(1); }
NVN(nvnCommandBufferGetMemoryCallbackData) { RET(cb_of(A(0))->callback_data); }
NVN(nvnCommandBufferSetCommandMemoryCallbackEnabled) { (void)c; }

// Reserve a record. May call back into the guest for more memory, so callers snapshot their
// arguments first. cb->at: the guest address it counts at.
static CmdRec *rec(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra) {
    uint32_t size = (uint32_t)((sizeof(CmdRec) + extra + 7) & ~7u), cost = guest_cost(extra);
    for (int tries = 0; !cb->arena || cb->used + cost + GUEST_RESERVE > cb->size; tries++) {
        if (!cb->callback || tries == 2) FATAL("nvn: out of command memory (need %u)\n", cost);
        uint64_t saved[8];
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = cb->guest;
        c->x[1] = 0;  // NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY
        c->x[2] = cost + GUEST_RESERVE;
        c->x[3] = cb->callback_data;
        guest_call(c, cb->callback);
        memcpy(c->x, saved, sizeof saved);
    }
    lists_overwritten(cb, cb->mem + cb->used, cb->mem + cb->used + cost);
    CmdRec *r = arena_rec(cb->arena, op, size);
    cb->at = cb->mem + cb->used;
    cb->used += cost;
    return r;
}

NVN(nvnCommandBufferBeginRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    cb->recording = true;
    CmdRec *r = rec(c, cb, CMD_BARRIER, 0);  // makes sure memory exists; the list starts here
    cb->start = cb->at;
    pthread_mutex_lock(&g_list_lock);
    int i = list_lower_bound(cb->start);
    if (i < g_nlists && g_lists[i].handle == cb->start) {  // (begun there before without new memory: replaced)
        Arena *was = g_lists[i].arena;
        was->live--;
        g_lists[i] = (ListStart){cb->start, r, cb->arena};
        if (was != cb->arena) arena_release(was);
    } else {
        if (g_nlists == g_lists_cap) {
            g_lists_cap = g_lists_cap ? g_lists_cap * 2 : 1024;
            g_lists = realloc(g_lists, g_lists_cap * sizeof *g_lists);
            if (!g_lists) FATAL("nvn: out of host memory for command lists\n");
        }
        memmove(&g_lists[i + 1], &g_lists[i], (g_nlists - i) * sizeof *g_lists);
        g_lists[i] = (ListStart){cb->start, r, cb->arena};
        g_nlists++;
    }
    atomic_fetch_add(&g_list_gen, 1);
    cb->arena->live++;
    pthread_mutex_unlock(&g_list_lock);
}
NVN(nvnCommandBufferEndRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    rec(c, cb, CMD_END, 0);
    cb->recording = false;
    plat_heap_check("EndRecording");
    if (getenv("ODYSSEY_LOG_LISTS"))
        LOG("[lists] cb %llx recorded %llx .. %llx (lr %llx)\n", (unsigned long long)A(0), (unsigned long long)cb->start,
            (unsigned long long)(cb->mem + cb->used), (unsigned long long)c->x[30]);
    RET(cb->start);
}
NVN(nvnCommandBufferIsRecording) { RET(cb_of(A(0))->recording); }

// ---- generic recorders
typedef struct { uint64_t x[8]; float f[4]; } Snap;
static Snap snap(Cpu *c) {
    Snap s;
    memcpy(s.x, c->x, sizeof s.x);
    for (int i = 0; i < 4; i++) s.f[i] = *(float *)&c->v[i];
    return s;
}
// The call's integer arguments as their NVN types say: only the low 32 bits of an int / enum and the low 8 of an
// NVNboolean are the argument (AAPCS64), the rest of the register can be anything - Odyssey's code leaves junk there
// (a disabled channel read as enabled, a binding's first slot as huge). One letter per argument from x1: p 64-bit
// (pointer, address, size), i int / enum, u unsigned 32-bit, b NVNboolean.
static void narrow(Snap *s, const char *types) {
    for (int i = 0; types[i] && i < 7; i++) {
        uint64_t *v = &s->x[i + 1];
        if (types[i] == 'i') *v = (uint64_t)(int64_t)(int32_t)*v;
        else if (types[i] == 'u') *v = (uint32_t)*v;
        else if (types[i] == 'b') *v = (uint8_t)*v != 0;
    }
}
static Snap snap_args(Cpu *c, const char *types) {
    Snap s = snap(c);
    narrow(&s, types);
    return s;
}
static CmdRec *record(Cpu *c, uint32_t op, uint32_t extra, Snap *s) {
    CmdRec *r = rec(c, cb_of(s->x[0]), op, extra);
    memcpy(r->x, s->x + 1, sizeof r->x);
    memcpy(r->f, s->f, sizeof r->f);
    return r;
}
// record with a copy of `len` bytes from the guest pointer in argument `arg`
static CmdRec *record_copy(Cpu *c, uint32_t op, int arg, uint32_t len, const char *types) {
    Snap s = snap_args(c, types);
    CmdRec *r = record(c, op, len, &s);
    if (s.x[arg] && len) memcpy(r->extra, (void *)(uintptr_t)s.x[arg], len);
    return r;
}
#define PLAIN(name, op, types) NVN(name) { Snap s = snap_args(c, types); record(c, op, 0, &s); }

// ---- state binding
NVN(nvnCommandBufferBindProgram) {
    Snap s = snap_args(c, "pi");
    CmdRec *r = record(c, CMD_BIND_PROGRAM, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnProgram, s.x[1]);  // host program
}
NVN(nvnCommandBufferBindBlendState) { record_copy(c, CMD_BIND_BLEND, 1, 8, "p"); }
NVN(nvnCommandBufferBindChannelMaskState) { record_copy(c, CMD_BIND_CHANNEL_MASK, 1, 4, "p"); }
NVN(nvnCommandBufferBindColorState) { record_copy(c, CMD_BIND_COLOR, 1, 4, "p"); }
NVN(nvnCommandBufferBindDepthStencilState) { record_copy(c, CMD_BIND_DEPTH_STENCIL, 1, 8, "p"); }
NVN(nvnCommandBufferBindMultisampleState) { record_copy(c, CMD_BIND_MULTISAMPLE, 1, 0x18, "p"); }
NVN(nvnCommandBufferBindPolygonState) { record_copy(c, CMD_BIND_POLYGON, 1, 4, "p"); }
NVN(nvnCommandBufferBindVertexAttribState) { record_copy(c, CMD_BIND_VERTEX_ATTRIB, 2, (uint32_t)A(1) * 4, "ip"); }
NVN(nvnCommandBufferBindVertexStreamState) { record_copy(c, CMD_BIND_VERTEX_STREAM, 2, (uint32_t)A(1) * 8, "ip"); }

// (cmd, first, count, const NVNbufferRange*) and the single-binding forms
NVN(nvnCommandBufferBindVertexBuffers) { record_copy(c, CMD_BIND_VERTEX_BUFFERS, 3, (uint32_t)A(2) * 16, "iip"); }
NVN(nvnCommandBufferBindVertexBuffer) {
    Snap s = snap_args(c, "ipp");
    CmdRec *r = record(c, CMD_BIND_VERTEX_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = 1;
    memcpy(r->extra, &s.x[2], 16);
}
// (cmd, stage, first, count, const NVNbufferRange*)
NVN(nvnCommandBufferBindUniformBuffers) { record_copy(c, CMD_BIND_UNIFORM_BUFFERS, 4, (uint32_t)A(3) * 16, "iiip"); }
NVN(nvnCommandBufferBindUniformBuffer) {
    Snap s = snap_args(c, "iipp");
    CmdRec *r = record(c, CMD_BIND_UNIFORM_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
NVN(nvnCommandBufferBindStorageBuffers) { record_copy(c, CMD_BIND_STORAGE_BUFFERS, 4, (uint32_t)A(3) * 16, "iiip"); }
NVN(nvnCommandBufferBindStorageBuffer) {
    Snap s = snap_args(c, "iipp");
    CmdRec *r = record(c, CMD_BIND_STORAGE_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
// (cmd, stage, first, count, const handle*)
NVN(nvnCommandBufferBindTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8, "iiip"); }
NVN(nvnCommandBufferBindTexture) {
    Snap s = snap_args(c, "iip");
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindImages) { record_copy(c, CMD_BIND_IMAGES, 4, (uint32_t)A(3) * 8, "iiip"); }
NVN(nvnCommandBufferBindImage) {
    Snap s = snap_args(c, "iip");
    CmdRec *r = record(c, CMD_BIND_IMAGES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8, "iiip"); }
NVN(nvnCommandBufferBindSeparateTexture) {
    Snap s = snap_args(c, "iip");
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateSamplers) { (void)c; }
NVN(nvnCommandBufferBindSeparateSampler) { (void)c; }
NVN(nvnCommandBufferBindTransformFeedbackBuffers) { (void)c; }
NVN(nvnCommandBufferBindTransformFeedbackBuffer) { (void)c; }
NVN(nvnCommandBufferBindCoverageModulationTable) { (void)c; }

NVN(nvnCommandBufferSetTexturePool) {
    Snap s = snap_args(c, "p");
    CmdRec *r = record(c, CMD_SET_TEXTURE_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}
NVN(nvnCommandBufferSetSamplerPool) {
    Snap s = snap_args(c, "p");
    CmdRec *r = record(c, CMD_SET_SAMPLER_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}

// SetRenderTargets(cmd, n, colors[], colorViews[], depth, depthView)
// extra: { u64 tex[8]; NvnTextureView views[8]; u8 has_view[8]; u64 depth; NvnTextureView dview; u8 has_dview }
typedef struct {
    uint64_t tex[8];
    NvnTextureView views[8];
    uint8_t has_view[8];
    uint64_t depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtRec;
NVN(nvnCommandBufferSetRenderTargets) {
    Snap s = snap_args(c, "ipppp");
    int n = (int)s.x[1];
    if (n > 8) n = 8;
    RtRec rt = {0};
    const uint64_t *colors = (const uint64_t *)(uintptr_t)s.x[2], *views = (const uint64_t *)(uintptr_t)s.x[3];
    for (int i = 0; i < n; i++) {
        rt.tex[i] = colors && colors[i] ? (uint64_t)(uintptr_t)HOST(NvnTexture, colors[i]) : 0;
        if (views && views[i]) { rt.has_view[i] = 1; memcpy(&rt.views[i], (void *)(uintptr_t)views[i], sizeof(NvnTextureView)); }
    }
    rt.depth = s.x[4] ? (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[4]) : 0;
    if (s.x[5]) { rt.has_dview = 1; memcpy(&rt.dview, (void *)(uintptr_t)s.x[5], sizeof(NvnTextureView)); }
    CmdRec *r = record(c, CMD_SET_RENDER_TARGETS, sizeof rt, &s);
    r->x[0] = (uint64_t)n;
    memcpy(r->extra, &rt, sizeof rt);
}

// viewports/scissors/depth ranges
NVN(nvnCommandBufferSetViewports) { record_copy(c, CMD_SET_VIEWPORTS, 3, (uint32_t)A(2) * 16, "iip"); }
NVN(nvnCommandBufferSetViewport) {
    Snap s = snap_args(c, "iiii");
    CmdRec *r = record(c, CMD_SET_VIEWPORTS, 16, &s);
    float v[4] = {(float)(int32_t)s.x[1], (float)(int32_t)s.x[2], (float)(int32_t)s.x[3], (float)(int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetScissors) { record_copy(c, CMD_SET_SCISSORS, 3, (uint32_t)A(2) * 16, "iip"); }
NVN(nvnCommandBufferSetScissor) {
    Snap s = snap_args(c, "iiii");
    CmdRec *r = record(c, CMD_SET_SCISSORS, 16, &s);
    int32_t v[4] = {(int32_t)s.x[1], (int32_t)s.x[2], (int32_t)s.x[3], (int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetDepthRanges) { record_copy(c, CMD_SET_DEPTH_RANGES, 3, (uint32_t)A(2) * 8, "iip"); }
NVN(nvnCommandBufferSetDepthRange) {
    Snap s = snap_args(c, "");
    CmdRec *r = record(c, CMD_SET_DEPTH_RANGES, 8, &s);
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, s.f, 8);
}
NVN(nvnCommandBufferSetBlendColor) { record_copy(c, CMD_SET_BLEND_COLOR, 1, 16, "p"); }
NVN(nvnCommandBufferSetStencilRef) { Snap s = snap_args(c, "ii"); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 0; }
NVN(nvnCommandBufferSetStencilMask) { Snap s = snap_args(c, "ii"); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 1; }
NVN(nvnCommandBufferSetStencilValueMask) { Snap s = snap_args(c, "ii"); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 2; }
PLAIN(nvnCommandBufferSetPrimitiveRestart, CMD_SET_PRIMITIVE_RESTART, "bi")
PLAIN(nvnCommandBufferSetAlphaRef, CMD_SET_ALPHA_REF, "")
PLAIN(nvnCommandBufferSetDepthBounds, CMD_SET_DEPTH_BOUNDS, "b")
PLAIN(nvnCommandBufferSetPolygonOffsetClamp, CMD_SET_POLYGON_OFFSET, "")
PLAIN(nvnCommandBufferSetLineWidth, CMD_SET_LINE_WIDTH, "")
PLAIN(nvnCommandBufferSetPointSize, CMD_SET_POINT_SIZE, "")
PLAIN(nvnCommandBufferSetSampleMask, CMD_SET_SAMPLE_MASK, "i")
PLAIN(nvnCommandBufferSetRenderEnable, CMD_SET_RENDER_ENABLE, "b")
NVN(nvnCommandBufferSetRenderEnableConditional) { (void)c; }
NVN(nvnCommandBufferSetDepthClamp) { (void)c; }
NVN(nvnCommandBufferSetRasterizerDiscard) { (void)c; }
NVN(nvnCommandBufferSetConservativeRasterEnable) { (void)c; }
NVN(nvnCommandBufferSetConservativeRasterDilate) { (void)c; }
NVN(nvnCommandBufferSetSubpixelPrecisionBias) { (void)c; }
NVN(nvnCommandBufferSetViewportSwizzles) { (void)c; }
NVN(nvnCommandBufferSetStencilCullCriteria) { (void)c; }
NVN(nvnCommandBufferSetColorReductionEnable) { (void)c; }
NVN(nvnCommandBufferSetColorReductionThresholds) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheAction) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheTileSize) { (void)c; }
NVN(nvnCommandBufferSetShaderScratchMemory) { (void)c; }
NVN(nvnCommandBufferSetPatchSize) { (void)c; }
NVN(nvnCommandBufferSetInnerTessellationLevels) { (void)c; }
NVN(nvnCommandBufferSetOuterTessellationLevels) { (void)c; }
NVN(nvnCommandBufferSetProgramSubroutines) { (void)c; }
NVN(nvnCommandBufferSaveZCullData) { (void)c; }
NVN(nvnCommandBufferRestoreZCullData) { (void)c; }

static uint64_t copy_row_stride, copy_image_stride;
RT_ON_RESET(cmdbuf_reset) { copy_row_stride = copy_image_stride = 0; }
NVN(nvnCommandBufferSetCopyRowStride) {
    Snap s = snap_args(c, "p");
    copy_row_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferSetCopyImageStride) {
    Snap s = snap_args(c, "p");
    copy_image_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferGetCopyRowStride) { RET(copy_row_stride); }
NVN(nvnCommandBufferGetCopyImageStride) { RET(copy_image_stride); }

// ---- clears
NVN(nvnCommandBufferClearColor) { record_copy(c, CMD_CLEAR_COLOR, 2, 16, "ipi"); }
NVN(nvnCommandBufferClearColori) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16, "ipi"); r->x[5] = 1; }
NVN(nvnCommandBufferClearColorui) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16, "ipi"); r->x[5] = 2; }
PLAIN(nvnCommandBufferClearDepthStencil, CMD_CLEAR_DEPTH_STENCIL, "bii")
// ClearTexture(cmd, texture, view, region, color, mask)
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } ClearTexRec;
static void clear_texture(Cpu *c, int kind) {
    Snap s = snap_args(c, "ppppi");
    ClearTexRec ct = {0};
    ct.tex = (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[1]);
    if (s.x[2]) { ct.has_view = 1; memcpy(&ct.view, (void *)(uintptr_t)s.x[2], sizeof ct.view); }
    if (s.x[3]) memcpy(ct.region, (void *)(uintptr_t)s.x[3], sizeof ct.region);
    if (s.x[4]) memcpy(ct.color, (void *)(uintptr_t)s.x[4], 16);
    CmdRec *r = record(c, CMD_CLEAR_TEXTURE, sizeof ct, &s);
    r->x[6] = (uint64_t)kind;
    memcpy(r->extra, &ct, sizeof ct);
}
NVN(nvnCommandBufferClearTexture) { clear_texture(c, 0); }
NVN(nvnCommandBufferClearTexturei) { clear_texture(c, 1); }
NVN(nvnCommandBufferClearTextureui) { clear_texture(c, 2); }
PLAIN(nvnCommandBufferClearBuffer, CMD_CLEAR_BUFFER, "ppu")
// DiscardColor(cmdbuf, index): the target's contents become undefined. On the Switch a discarded (compressed)
// target reads back cleared; areas the game then leaves undrawn (e.g. a pit after a floor tile falls away) stay
// black there. Treated as a clear to (0,0,0,0) (ODYSSEY_KEEP_DISCARD=1: old no-op behaviour).
NVN(nvnCommandBufferDiscardColor) {
    static int keep = -1, logged;
    if (keep < 0) keep = getenv("ODYSSEY_KEEP_DISCARD") != NULL;
    if (logged < 5) { logged++; LOG("[nvn] DiscardColor index %llu%s\n", (unsigned long long)c->x[1], keep ? " (kept)" : " -> clear"); }
    if (keep) return;
    Snap s = snap_args(c, "i");
    CmdRec *r = record(c, CMD_CLEAR_COLOR, 16, &s);
    memset(r->extra, 0, 16);
    r->x[2] = 0xF;  // all channels
    r->x[5] = 0;    // float clear
}
NVN(nvnCommandBufferDiscardDepthStencil) { (void)c; }

// ---- draws (all integer arguments fit in x1..x7)
NVN(nvnCommandBufferDrawArrays) { Snap s = snap_args(c, "iii"); CmdRec *r = record(c, CMD_DRAW_ARRAYS, 0, &s); r->x[3] = 0; r->x[4] = 1; }
NVN(nvnCommandBufferDrawArraysInstanced) { Snap s = snap_args(c, "iiiii"); record(c, CMD_DRAW_ARRAYS, 0, &s); }
// Debug: ODYSSEY_TRACE_COUNT=<n> logs the guest call chain of indexed draws with n indices.
static void trace_draw(Cpu *c, uint64_t count) {
    static long want = -2;
    if (want == -2) want = getenv("ODYSSEY_TRACE_COUNT") ? atol(getenv("ODYSSEY_TRACE_COUNT")) : -1;
    if (want < 0 || (long)count != want) return;
    static int n;
    if (n++ > 40) return;
    LOG("[trace] draw count %llu lr %llx chain:", (unsigned long long)count, (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 14 && fp >= 0x7000000000ull && fp < 0x8000000000ull && (fp & 7) == 0; i++) {
        uint64_t next = *(uint64_t *)(uintptr_t)fp, lr = *(uint64_t *)(uintptr_t)(fp + 8);
        LOG(" %llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    LOG("\n");
}

NVN(nvnCommandBufferDrawElements) {
    trace_draw(c, c->x[3]);
    Snap s = snap_args(c, "iiip");
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[4] = 0; r->x[5] = 0; r->x[6] = 1;  // base vertex, base instance, instances
}
NVN(nvnCommandBufferDrawElementsBaseVertex) {
    trace_draw(c, c->x[3]);
    Snap s = snap_args(c, "iiipi");
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[5] = 0; r->x[6] = 1;
}
NVN(nvnCommandBufferDrawElementsInstanced) { trace_draw(c, c->x[3]); Snap s = snap_args(c, "iiipiii"); record(c, CMD_DRAW_ELEMENTS, 0, &s); }
PLAIN(nvnCommandBufferDrawArraysIndirect, CMD_DRAW_ARRAYS_INDIRECT, "ip")
PLAIN(nvnCommandBufferDrawElementsIndirect, CMD_DRAW_ELEMENTS_INDIRECT, "iipp")
NVN(nvnCommandBufferMultiDrawArraysIndirectCount) { (void)c; }
NVN(nvnCommandBufferMultiDrawElementsIndirectCount) { (void)c; }
NVN(nvnCommandBufferDrawTransformFeedback) { (void)c; }
NVN(nvnCommandBufferDrawTexture) { (void)c; }
PLAIN(nvnCommandBufferDispatchCompute, CMD_DISPATCH, "iii")
PLAIN(nvnCommandBufferDispatchComputeIndirect, CMD_DISPATCH_INDIRECT, "p")
NVN(nvnCommandBufferBeginTransformFeedback) { (void)c; }
NVN(nvnCommandBufferEndTransformFeedback) { (void)c; }
NVN(nvnCommandBufferPauseTransformFeedback) { (void)c; }
NVN(nvnCommandBufferResumeTransformFeedback) { (void)c; }

// ---- copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRef;
static void tex_ref(TexRef *t, uint64_t tex, uint64_t view, uint64_t region) {
    t->tex = tex ? (uint64_t)(uintptr_t)HOST(NvnTexture, tex) : 0;
    if (view) { t->has_view = 1; memcpy(&t->view, (void *)(uintptr_t)view, sizeof t->view); }
    if (region) memcpy(t->region, (void *)(uintptr_t)region, sizeof t->region);
}
NVN(nvnCommandBufferCopyBufferToTexture) {  // (cmd, src, dst, dstView, dstRegion, flags)
    Snap s = snap_args(c, "ppppi");
    TexRef t = {0};
    tex_ref(&t, s.x[2], s.x[3], s.x[4]);
    CmdRec *r = record(c, CMD_COPY_BUFFER_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToBuffer) {  // (cmd, src, srcView, srcRegion, dst, flags)
    Snap s = snap_args(c, "ppppi");
    TexRef t = {0};
    tex_ref(&t, s.x[1], s.x[2], s.x[3]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_BUFFER, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToTexture) {  // (cmd, src, srcView, srcRegion, dst, dstView, dstRegion, flags)
    Snap s = snap_args(c, "ppppppi");
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], s.x[3]);
    tex_ref(&t[1], s.x[4], s.x[5], s.x[6]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
PLAIN(nvnCommandBufferCopyBufferToBuffer, CMD_COPY_BUFFER_TO_BUFFER, "pppi")
NVN(nvnCommandBufferDownsample) {
    Snap s = snap_args(c, "pp");
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], 0, 0);
    tex_ref(&t[1], s.x[2], 0, 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferDownsampleTextureView) {
    Snap s = snap_args(c, "pppp");
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], 0);
    tex_ref(&t[1], s.x[3], s.x[4], 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferTiledDownsample) { (void)c; }
NVN(nvnCommandBufferTiledDownsampleTextureView) { (void)c; }
NVN(nvnCommandBufferResolveDepthBuffer) { (void)c; }
// UpdateUniformBuffer(cmd, buffer, size, offset, dataSize, data)
NVN(nvnCommandBufferUpdateUniformBuffer) { record_copy(c, CMD_UPDATE_UNIFORM_BUFFER, 5, (uint32_t)A(4), "ppppp"); }

// ---- synchronization and misc
NVN(nvnCommandBufferFenceSync) {
    // work executes at submit time, so the sync is satisfied once this list has run
    Snap s = snap_args(c, "pii");
    record(c, CMD_FENCE_SYNC, 0, &s);
}
PLAIN(nvnCommandBufferWaitSync, CMD_WAIT_SYNC, "p")
PLAIN(nvnCommandBufferBarrier, CMD_BARRIER, "i")
PLAIN(nvnCommandBufferSignalEvent, CMD_SIGNAL_EVENT, "piiiu")
PLAIN(nvnCommandBufferWaitEvent, CMD_WAIT_EVENT, "piu")
PLAIN(nvnCommandBufferReportCounter, CMD_REPORT_COUNTER, "ip")
NVN(nvnCommandBufferReportValue) { (void)c; }
NVN(nvnCommandBufferResetCounter) { (void)c; }
NVN(nvnCommandBufferCallCommands) { record_copy(c, CMD_CALL, 2, (uint32_t)A(1) * 8, "ip"); }
// CopyCommands(cmd, count, const handles*): the lists' commands themselves, as the driver copies them - the source
// memory may be reused right after (a call would run whatever was recorded there next).
NVN(nvnCommandBufferCopyCommands) {
    NvnCmdBuf *cb = cb_of(A(0));
    uint64_t n = (uint32_t)A(1), *handles = malloc((n ? n : 1) * 8);
    memcpy(handles, AP(2), n * 8);
    for (uint64_t i = 0; i < n; i++)
        for (CmdRec *r = cmd_list(handles[i]); r && r->op != CMD_END;) {
            if (r->op == CMD_JUMP) { r = (CmdRec *)(uintptr_t)r->x[0]; continue; }
            CmdRec *d = rec(c, cb, r->op, r->size - (uint32_t)sizeof(CmdRec));
            memcpy(d, r, r->size);
            r = (CmdRec *)((uint8_t *)r + r->size);
        }
    free(handles);
    plat_heap_check("CopyCommands");
}
NVN(nvnCommandBufferInsertDebugMarker) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerDynamic) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerStatic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroup) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupDynamic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupStatic) { (void)c; }
NVN(nvnCommandBufferPopDebugGroup) { (void)c; }
NVN(nvnCommandBufferPopDebugGroupId) { (void)c; }
