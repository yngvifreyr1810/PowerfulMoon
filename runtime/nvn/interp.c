// Frame interpolation: images between the game's own frames (Video tab "Frame interpolation", off by default).
//
// The game steps its world once per frame, so smoother motion than its frame rate needs images it never made. Each
// frame's command lists are run a second time (more for more images) with the uniforms of each draw blended between
// the previous frame and this one - matrices, colours, animation - as RT64 does for N64 games. The backend
// (vulkan/device.c) runs the matching and the replay on a thread of its own and shows each in-between half a game frame
// before the game's own image.
//
// Draws are matched across frames by programs + draw arguments + target size (the game's render-target objects and
// dynamic buffer addresses change every frame; this matched 96-100% of draws in Mabe Village, Tail Cave, a door
// transition, a pit); several draws of one key (the same mesh for different objects: Cuccos, chain links) are paired
// by the nearest per-object uniforms, not their order, which the game's depth sort changes between frames
// (match_all). Every uniform buffer of a matched draw is paired by position and blended element-wise; integers
// (stored in float slots they read as denormals) never are, nor whole numbers that changed by one or more (a texture's
// frame, a mode: this frame's), and a texture translation that wrapped (0.98 -> 0.01) blends the short way round. An element moving further than the cut distance (+5% of
// its size: projection terms are large) is a jump - it shows its new value at once (a camera's settings switching as
// Link enters another part of the overworld: one value of its buffer). A jump in a shared block (the camera, a pass:
// a camera cut), jumps in most of the buffers that changed (a warp), a level change, too few matches or a texture
// going away during the frame make the whole frame a cut: no in-between images, the frame shows as it is. The
// shadow pass's light block and its fragment-stage copies snap together, as one light.
//
//   ODYSSEY_INTERP=on|measure|off   override the setting (measure: log and match only, no extra images)
//   ODYSSEY_INTERP_CUT=<d>          the jump distance (default 300 world units: Odyssey's centimetres)
//   ODYSSEY_INTERP_STAGES=<m>       which stages' uniforms are blended: 1 vertex, 2 fragment, 3 both (default 3: with
//                                    the vertex stage's only, the shadow maps - drawn again with blended light matrices -
//                                    were read with the game frame's matrices in the fragment stage: Mabe Village's
//                                    cast shadows faded / dithered in the in-betweens)
//   ODYSSEY_INTERP_LOG=<s>          stats line period in seconds (default 2; 0: none)
//   ODYSSEY_INTERP_LOG_CUTS=1       log each frame cut and why
//   ODYSSEY_INTERP_SNAP=elem        a jumping buffer snaps per element (default: the whole buffer shows this frame)
//   ODYSSEY_INTERP_TOGGLE=0         blend values that alternate every frame too (default: they keep this frame's value)
//   research: ODYSSEY_INTERP_LOG_MOVES=<d> [_FROM=<frame>] logs uniform buffers whose elements moved more than d in one
//   frame; ODYSSEY_INTERP_DUMP=<frame>:<path> writes both frames' draws and uniform copies as text at that frame;
//   ODYSSEY_INTERP_LOG_KEYS=1 (renderer.m) logs each draw key's components once
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../rt.h"
#include "../platform.h"
#include "../settings.h"
#include "interp.h"

bool g_interp_log, g_interp_replaying;
uint64_t g_interp_present_frame;

// (the logs' level name: Odyssey keeps no such global - its stage scene is logged by hle/odyssey_warp.c)
const char *game_level_name(void) { return "-"; }

int interp_wanted(void) {
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("ODYSSEY_INTERP");
        env = !e || !*e ? -1 : !strcmp(e, "measure") ? INTERP_MEASURE
            : (!strcmp(e, "0") || !strcmp(e, "off")) ? INTERP_OFF : INTERP_ON;
        if (env >= 0) LOG("[interp] ODYSSEY_INTERP=%s\n", e);
    }
    if (env >= 0) return env;
#ifdef __APPLE__
    return INTERP_OFF;  // (the Vulkan backend's so far: vulkan/exec.c, device.c)
#else
    return g_settings.frame_interp ? INTERP_ON : INTERP_OFF;
#endif
}

typedef struct {
    uint8_t stage, slot, snap;  // snap: elements past the jump distance show the new value (a jump, not motion)
    uint32_t off, size;
    uint32_t used;  // the bytes the shader reads: compared and blended; the rest (unwritten leftovers) are this frame's
    uint64_t addr;  // the guest address of the buffer (research: is it an object's identity across frames?)
    int64_t prev_off, blend;
    int64_t prev2_off;  // the same buffer two frames back (-1: none): an element back at that value toggles, not moves
    float maxd;  // the largest element change from the previous frame (-1: not matched): how fast it was moving
} CbRec;
typedef struct {
    uint64_t key; uint32_t occ, first_cb, ncb;
    uint8_t depth_only;  // drawn into a depth-only target (a shadow pass: its shared blocks are the light's)
    uint8_t screen_pass; // a lone draw without per-object blocks, paired (match_all): a full-screen pass
    uint64_t obj;   // the object: the guest addresses of its per-object uniform buffers (0: none - UI); frame_end
    uint32_t oocc;  // occurrence of (obj, key) in the frame
    int32_t pair;   // the draw's partner in the frame before (-1 none; -2 UI, not blended); frame_end
} DrawRec;
typedef struct { uint64_t addr, size, off; } StoRec;  // a storage range read through gmem: its bytes at the time, in `bytes`
typedef struct {
    StoRec *sto; uint32_t nsto, cap_sto;
    struct { uint64_t addr, size; } *dout; uint32_t ndout, cap_dout;  // the frame's dispatch outputs
    DrawRec *draws; uint32_t ndraws, cap_draws;
    CbRec *cbs; uint32_t ncbs, cap_cbs;
    uint8_t *bytes; uint64_t nbytes, cap_bytes;
    uint64_t *handles; uint32_t nhandles, cap_handles;
    bool overflow, destroyed;
    uint64_t free_gen;  // interp_free_begin's count when the frame's first list ran
    uint64_t frame_no;  // the game's present it was closed at (interp_frame_close)
    uint8_t *state; size_t state_n, state_cap;  // the backend's executor state at its first list (a replay starts there)
    struct { uint32_t off; float maxd; } *moves;  // each logged copy's largest element change from the frame before (moves_put)
    struct { uint8_t stage, slot; uint32_t size; float maxd; } cls[16];  // the same for the shared blocks by kind (cls_put)
    int ncls;
    bool busy;          // (fr_lock) a slot in use
} Frame;

// The game's thread logs into `lg`; at its present the frame is closed (interp_frame_close) and handed to the
// interpolation thread (the backend's), which matches it against the two before it (interp_frame_begin: cur, prev, prev2),
// replays it and rotates (interp_frame_rotate) - so the matching and the replays of frame N run while the game works on
// N + 1. Slots: the logging one, two queued, the interpolation thread's three, one spare.
#define NFR 7
static Frame fr[NFR] = {[0] = {.busy = true}, [1] = {.busy = true}, [2] = {.busy = true}};
static Frame *lg = &fr[0], *cur, *prev = &fr[1], *prev2 = &fr[2];  // (prev2: the frame before prev - object identity, below)
static pthread_mutex_t fr_lock = PTHREAD_MUTEX_INITIALIZER;
static bool prev_valid, frame_open, force_cut, can_blend;
static uint8_t *blend_bytes; static uint64_t blend_n, blend_cap;

#define GROW(p, n, cap, init) do { if ((n) >= (cap)) { (cap) = (cap) ? (cap) * 2 : (init); (p) = realloc((p), sizeof *(p) * (cap)); } } while (0)

// occurrence counting / matching: open addressing on (key, occ)
#define HBITS 15
static uint64_t occ_keys[1 << HBITS]; static uint32_t occ_cnt[1 << HBITS];
static uint64_t m_keys[1 << HBITS]; static uint32_t m_occ[1 << HBITS]; static int32_t m_idx[1 << HBITS];

static uint32_t next_occ(uint64_t key) {
    uint32_t h = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> (64 - HBITS));
    for (int i = 0; i < (1 << HBITS); i++, h = (h + 1) & ((1 << HBITS) - 1)) {
        if (occ_keys[h] == key) return occ_cnt[h]++;
        if (!occ_keys[h]) { occ_keys[h] = key; occ_cnt[h] = 1; return 0; }
    }
    return 0;
}
// Object identity (Dusklight keys interpolation by the address of an object's own matrix storage). The game double-
// buffers each object's uniform blocks by frame parity: a draw's per-object buffers (vertex stage, slots 2 and up) sit
// at the same guest addresses as that object's draw two frames earlier, never the frame before (measured in Mabe
// Village: 49 of 49 static objects' addresses equal to N-2's, 0 to N-1's). So the draw of frame N finds its object in
// frame N-2 by address, and that draw's partner in N-1 - paired the frame before - is its own partner now. Objects
// without one in N-2 (just appeared, or a cut frame between) fall back to the nearest per-object uniforms (match_all).
static uint64_t i_keys[1 << HBITS]; static uint32_t i_occ[1 << HBITS]; static int32_t i_idx[1 << HBITS];
static uint64_t ihash(uint64_t obj, uint64_t key, uint32_t occ) {
    return (obj * 0xff51afd7ed558ccdull ^ key ^ (uint64_t)occ * 0xc4ceb9fe1a85ec53ull) * 0x9E3779B97F4A7C15ull;
}
static void index_prev2(void) {
    memset(i_idx, 0xff, sizeof i_idx);
    for (uint32_t j = 0; j < prev2->ndraws && j < (1u << HBITS) / 2; j++) {
        DrawRec *d = &prev2->draws[j];
        if (!d->obj) continue;
        uint32_t h = (uint32_t)(ihash(d->obj, d->key, d->oocc) >> (64 - HBITS));
        while (i_idx[h] >= 0) h = (h + 1) & ((1 << HBITS) - 1);
        i_keys[h] = ihash(d->obj, d->key, d->oocc), i_occ[h] = d->oocc, i_idx[h] = (int32_t)j;
    }
}
static int32_t lookup_prev2(uint64_t obj, uint64_t key, uint32_t oocc) {
    uint64_t ik = ihash(obj, key, oocc);
    uint32_t h = (uint32_t)(ik >> (64 - HBITS));
    for (int i = 0; i < (1 << HBITS); i++, h = (h + 1) & ((1 << HBITS) - 1)) {
        if (i_idx[h] < 0) return -1;
        if (i_keys[h] == ik && i_occ[h] == oocc) return i_idx[h];
    }
    return -1;
}
// The objects of this frame's draws and their occurrence per (obj, key): from the logged buffers' addresses.
static uint64_t o_keys[1 << HBITS]; static uint32_t o_cnt[1 << HBITS];
static void assign_objects(Frame *F) {
    memset(o_keys, 0, sizeof o_keys);
    for (uint32_t j = 0; j < F->ndraws; j++) {
        DrawRec *d = &F->draws[j];
        uint64_t obj = 0;
        for (uint32_t i = 0; i < d->ncb; i++) {
            CbRec *c = &F->cbs[d->first_cb + i];
            if (c->stage == 0 && c->slot >= 2 && c->size >= 48) obj = (obj ^ c->addr ^ (uint64_t)c->slot << 56) * 0x9E3779B97F4A7C15ull;
        }
        d->obj = obj;
        d->oocc = 0;
        if (!obj) continue;
        uint64_t ok = ihash(obj, d->key, 0) | 1;
        uint32_t h = (uint32_t)(ok >> (64 - HBITS));
        for (int i = 0; i < (1 << HBITS); i++, h = (h + 1) & ((1 << HBITS) - 1)) {
            if (o_keys[h] == ok) { d->oocc = o_cnt[h]++; break; }
            if (!o_keys[h]) { o_keys[h] = ok; o_cnt[h] = 1; break; }
        }
    }
}

static uint32_t mhash(uint64_t key, uint32_t occ) {
    return (uint32_t)((key ^ (uint64_t)occ * 0xff51afd7ed558ccdull) * 0x9E3779B97F4A7C15ull >> (64 - HBITS));
}
static int32_t lookup_prev(uint64_t key, uint32_t occ) {
    uint32_t h = mhash(key, occ);
    for (int i = 0; i < (1 << HBITS); i++, h = (h + 1) & ((1 << HBITS) - 1)) {
        if (m_idx[h] < 0) return -1;
        if (m_keys[h] == key && m_occ[h] == occ) return m_idx[h];
    }
    return -1;
}
static void index_prev(void) {
    memset(m_idx, 0xff, sizeof m_idx);
    for (uint32_t j = 0; j < prev->ndraws && j < (1u << HBITS) / 2; j++) {
        uint32_t h = mhash(prev->draws[j].key, prev->draws[j].occ);
        while (m_idx[h] >= 0) h = (h + 1) & ((1 << HBITS) - 1);
        m_keys[h] = prev->draws[j].key, m_occ[h] = prev->draws[j].occ, m_idx[h] = (int32_t)j;
    }
}

// Several draws of one key in a frame are the same mesh drawn for different objects (two Cuccos, BowWow's chain links,
// grass tufts) - or the same object drawn in several passes. The game's order among them changes from frame to frame
// (it sorts by depth: as the camera moves, two objects swap), so pairing them by occurrence blends one object's uniforms
// with another's: the in-between shows the object half-way between the two, and its shadow with it (Mabe Village,
// running east past the well: a Cucco on the path that was in the yard, BowWow gone, a dark wedge of shadow on the
// grass). Each is paired with the draw of the previous frame whose per-object uniforms are nearest to its own instead.
static uint8_t *p_used; static uint32_t p_used_cap;
static bool blendable(float a, float b);
static float cut_threshold(void);
static bool history_reset;  // a forced cut (a level change, a resize): the object identity chain starts over
// How far apart two draws' objects are: the per-object uniforms (vertex stage, slots 2 and up: the model matrix, the
// bones). The shared buffers (slots 0, 1: the pass, the camera) say nothing about the object and their unwritten tails
// hold leftovers that differ from frame to frame (HUD quads: -3e38, 255, 1268 in slot 0's last rows).
static float draw_distance(const DrawRec *d, const DrawRec *p) {
    float dist = 0;
    for (uint32_t i = 0; i < d->ncb && i < p->ncb; i++) {
        const CbRec *cc = &cur->cbs[d->first_cb + i], *pc = &prev->cbs[p->first_cb + i];
        if (cc->stage != pc->stage || cc->slot != pc->slot || cc->size != pc->size) return 1e30f;
        if (cc->stage != 0 || cc->slot < 2) continue;
        const float *a = (const float *)(prev->bytes + pc->off), *b = (const float *)(cur->bytes + cc->off);
        if (!memcmp(a, b, cc->used)) continue;
        for (uint32_t q = 0; q < cc->used / 4; q++)
            if (blendable(a[q], b[q])) dist += fabsf(b[q] - a[q]);
            else if (a[q] != b[q]) dist += 1;  // (an integer / flag changed: another object)
    }
    return dist;
}
// The largest per-object element change between two draws: past the cut distance they are not one object that moved
// but one that left and one that appeared (Dusklight, RT64: no match - drawn as it is, never blended with another).
static float draw_maxdiff(const DrawRec *d, const DrawRec *p) {
    float m = 0;
    for (uint32_t i = 0; i < d->ncb && i < p->ncb; i++) {
        const CbRec *cc = &cur->cbs[d->first_cb + i], *pc = &prev->cbs[p->first_cb + i];
        if (cc->stage != 0 || cc->slot < 2 || pc->stage != cc->stage || pc->slot != cc->slot || pc->size != cc->size) continue;
        const float *a = (const float *)(prev->bytes + pc->off), *b = (const float *)(cur->bytes + cc->off);
        if (!memcmp(a, b, cc->used)) continue;
        for (uint32_t q = 0; q < cc->used / 4; q++)
            if (blendable(a[q], b[q])) { float dd = fabsf(b[q] - a[q]) - 0.05f * fmaxf(fabsf(a[q]), fabsf(b[q])); if (dd > m) m = dd; }
    }
    return m;
}
static uint32_t object_bytes(const DrawRec *d) {  // the per-object uniforms a draw has (none: HUD quads, decals)
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->ncb; i++) {
        const CbRec *cc = &cur->cbs[d->first_cb + i];
        if (cc->stage == 0 && cc->slot >= 2) n += cc->size;
    }
    return n;
}

// The pairing of a key's draws: every (this frame, previous frame) pair of the key by distance, nearest first, each draw
// once (greedy in that order: the draws that didn't change pair first; one whose content jumped takes what is left,
// its own, instead of taking another's and starting a chain of wrong pairs). Equal distances - the same object drawn
// in two passes (depth pre-pass and colour pass: identical per-object data, different per-pass buffers) - keep the
// occurrence order: crossing those blended one pass's camera / light buffer with the other's (whole scene flat-lit).
// The per-object uniforms (what draw_distance compares) as one number: an unchanged object finds its draw of the frame
// before among hundreds (match_all; draw_distance confirms)
// How many of this frame's draws bind each per-object copy: one an object's passes share (its matrices) has a few;
// one bound by hundreds (a material or scene block in a high slot) is no object's - one decision per object
// (frame_end) must not spread through it.
#define UBITS 14
static struct { uint32_t off, n; } cb_uses[1 << UBITS];
static void uses_build(void) {
    memset(cb_uses, 0, sizeof cb_uses);
    for (uint32_t i = 0; i < cur->ncbs; i++) {
        const CbRec *c = &cur->cbs[i];
        if (c->stage > 1 || c->slot < 2) continue;
        uint32_t h = (c->off * 0x9E3779B1u) >> (32 - UBITS);
        for (int k = 0; k < 64; k++, h = (h + 1) & ((1 << UBITS) - 1)) {
            if (cb_uses[h].off == c->off + 1) { cb_uses[h].n++; break; }
            if (!cb_uses[h].off) { cb_uses[h].off = c->off + 1, cb_uses[h].n = 1; break; }
        }
    }
}
static bool few_uses(const CbRec *c) {
    uint32_t h = (c->off * 0x9E3779B1u) >> (32 - UBITS);
    for (int k = 0; k < 64 && cb_uses[h].off; k++, h = (h + 1) & ((1 << UBITS) - 1))
        if (cb_uses[h].off == c->off + 1) return cb_uses[h].n <= 6;
    return false;
}

typedef struct { uint64_t h; uint32_t q; } ObjHash;
static int ph_cmp(const void *a, const void *b) {
    const ObjHash *x = a, *y = b;
    return x->h != y->h ? (x->h < y->h ? -1 : 1) : (x->q < y->q ? -1 : x->q > y->q);
}
static uint64_t object_hash(const Frame *F, const DrawRec *d) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < d->ncb; i++) {
        const CbRec *c = &F->cbs[d->first_cb + i];
        h = (h ^ ((uint64_t)c->stage << 40 | (uint64_t)c->slot << 32 | c->size)) * 0x100000001b3ull;
        if (c->stage != 0 || c->slot < 2) continue;
        const uint8_t *p = F->bytes + c->off;
        uint32_t q = 0;
        for (; q + 8 <= c->used; q += 8) { uint64_t w; memcpy(&w, p + q, 8); h = (h ^ w) * 0x100000001b3ull; }
        for (; q < c->used; q++) h = (h ^ p[q]) * 0x100000001b3ull;
    }
    return h;
}

typedef struct { float d; uint16_t c, p, docc; } Pair;
static int pair_cmp(const void *a, const void *b) {
    const Pair *x = a, *y = b;
    if (x->d != y->d) return x->d < y->d ? -1 : 1;
    return x->docc < y->docc ? -1 : x->docc > y->docc;
}
static int32_t *p_match;  // per draw of this frame: its previous-frame draw (-1: none; -2: UI; -3: just appeared - not blended)
static uint64_t st_id_pairs, st_content_pairs, st_new_objects, st_same_pairs, st_occ_pairs, s_same_pairs, s_occ_pairs, s_id_pairs, s_content_pairs, s_new_objects, s_class_snaps, s_toggled, s_integral, s_wraps, s_discont;  // (stats)
static uint32_t *p_order; static uint32_t p_order_cap;
static int order_cmp(const void *a, const void *b) {
    const DrawRec *x = &cur->draws[*(const uint32_t *)a], *y = &cur->draws[*(const uint32_t *)b];
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->occ < y->occ ? -1 : x->occ > y->occ;
}
#define GROUP_MAX 64
static void match_all(void) {
    if (cur->ndraws > p_order_cap) {
        p_order_cap = cur->cap_draws;
        p_order = realloc(p_order, sizeof *p_order * p_order_cap);
        p_match = realloc(p_match, sizeof *p_match * p_order_cap);
    }
    if (prev->ndraws > p_used_cap) { p_used_cap = prev->cap_draws; p_used = realloc(p_used, p_used_cap); }
    memset(p_used, 0, prev->ndraws);
    for (uint32_t j = 0; j < cur->ndraws; j++) p_order[j] = j, p_match[j] = -1;
    // by identity: the object's draw in prev2 (same per-object buffer addresses) and that draw's partner in prev
    st_id_pairs = st_content_pairs = st_new_objects = st_same_pairs = st_occ_pairs = 0;
    if (prev2->ndraws && !prev2->overflow && !history_reset) {
        index_prev2();
        static int32_t *inv; static uint32_t inv_cap;  // prev2's draw -> prev's draw paired with it
        if (prev2->ndraws > inv_cap) { inv_cap = prev2->cap_draws; inv = realloc(inv, sizeof *inv * inv_cap); }
        for (uint32_t k = 0; k < prev2->ndraws; k++) inv[k] = -1;
        for (uint32_t k = 0; k < prev->ndraws; k++) if (prev->draws[k].pair >= 0 && (uint32_t)prev->draws[k].pair < prev2->ndraws) inv[prev->draws[k].pair] = (int32_t)k;
        for (uint32_t j = 0; j < cur->ndraws; j++) {
            DrawRec *d = &cur->draws[j];
            if (!d->obj) continue;
            int32_t y = lookup_prev2(d->obj, d->key, d->oocc);
            if (y < 0) continue;
            int32_t x = inv[y];  // prev's draw paired with y
            // (same pass of the object: its occurrence within (obj, key) - a pre-pass / colour-pass crossing made once
            // by a tie would otherwise be carried along the parity chain for ever: flat-lit images every other frame)
            if (x < 0 || p_used[x] || prev->draws[x].key != d->key || prev->draws[x].oocc != d->oocc) continue;
            // (the game hands its per-object uniform blocks out per frame in draw order for some objects - Tostarena's
            // ice and its neighbours: turning the camera reorders them, and the "same" addresses held another object,
            // thousands of units away. Paired by address anyway, that draw jumped (drawn as this frame has it) while
            // the object's other pass blended: its depth-equal colour pass failed along the edges - holes and bright
            // lines in the ice, flicker at every in-between. A partner that far is found by content instead.)
            if (draw_maxdiff(d, &prev->draws[x]) > cut_threshold()) continue;
            p_match[j] = x;
            p_used[x] = 1;
            st_id_pairs++;
        }
    }
    qsort(p_order, cur->ndraws, sizeof *p_order, order_cmp);
    static Pair pairs[GROUP_MAX * GROUP_MAX];
    for (uint32_t g = 0; g < cur->ndraws;) {
        uint64_t key = cur->draws[p_order[g]].key;
        uint32_t ge = g + 1;
        while (ge < cur->ndraws && cur->draws[p_order[ge]].key == key) ge++;
        static int32_t *cand;
        static uint32_t cand_cap;
        uint32_t nprev = 0;
        for (;; nprev++) {
            int32_t pj = lookup_prev(key, nprev);
            if (pj < 0) break;
            if (nprev == cand_cap) cand = realloc(cand, sizeof *cand * (cand_cap = cand_cap ? cand_cap * 2 : 256));
            cand[nprev] = pj;
        }
        // Draws without per-object uniforms (vertex stage, slots 2 and up) are the HUD's and the menus' quads (and a
        // few static full-screen passes): they are not blended at all - an in-between shows them as this frame draws
        // them. Their shared buffers end in unwritten leftovers that differ every frame, so no distance tells them
        // apart, and their order shifts whenever one is added (the Items menu's cursor moving a slot: every later quad
        // paired with its neighbour, the whole grid scrambled for an image). The HUD moving at the game's own 60 Hz is
        // not what the in-betweens are for.
        uint32_t left = 0;  // (draws of the group not paired by identity)
        for (uint32_t k = g; k < ge; k++) left += p_match[p_order[k]] == -1;
        if (object_bytes(&cur->draws[p_order[g]]) < 48) {
            // ... except a lone one: a full-screen pass (the shadow mask, the lighting, fog) whose pass block holds the
            // camera's inverse and the shadow matrices - it reads the in-between's depth, so it takes the in-between's
            // camera too. Unblended, it unprojected that depth with this frame's: shadows cast where nothing stands
            // (Tostarena: a shop keeper's long dark silhouette on the sand while the camera turned).
            if (ge - g == 1 && nprev == 1 && !p_used[cand[0]] && p_match[p_order[g]] == -1) {
                p_match[p_order[g]] = cand[0], p_used[cand[0]] = 1, st_content_pairs++;
                cur->draws[p_order[g]].screen_pass = 1;
            } else
                for (uint32_t k = g; k < ge; k++) p_match[p_order[k]] = -2;
        } else if (left && nprev) {
            // Unchanged first: a draw whose per-object uniforms are byte for byte those of a draw of the frame before
            // is that object (static: most of a town), in the same pass of it (occurrence within obj + key). Before, a
            // key with more than GROUP_MAX draws (Tostarena's houses and palms with their shadow passes) was paired by
            // occurrence, and the game's depth sort reorders them as the camera turns: the in-betweens blended one
            // object with another (the town flickered while the camera moved).
            static ObjHash *ph;
            static uint32_t ph_cap;
            if (nprev > ph_cap) ph = realloc(ph, sizeof *ph * (ph_cap = nprev * 2));
            uint32_t nph = 0;
            for (uint32_t q = 0; q < nprev; q++)
                if (!p_used[cand[q]]) ph[nph].h = object_hash(prev, &prev->draws[cand[q]]), ph[nph++].q = q;
            qsort(ph, nph, sizeof *ph, ph_cmp);
            for (uint32_t k = g; k < ge && nph; k++) {
                uint32_t j = p_order[k];
                if (p_match[j] != -1) continue;
                const DrawRec *dc = &cur->draws[j];
                uint64_t h = object_hash(cur, dc);
                uint32_t lo = 0, hi = nph;
                while (lo < hi) { uint32_t m = (lo + hi) / 2; if (ph[m].h < h) lo = m + 1; else hi = m; }
                for (uint32_t i = lo; i < nph && ph[i].h == h; i++) {
                    int32_t pj = cand[ph[i].q];
                    if (p_used[pj] || prev->draws[pj].oocc != dc->oocc || draw_distance(dc, &prev->draws[pj]) != 0) continue;
                    p_match[j] = pj, p_used[pj] = 1, st_same_pairs++;
                    break;
                }
            }
            // The rest - objects that moved - by distance, nearest first (past GROUP_MAX each way: by occurrence)
            uint32_t rc[GROUP_MAX], rq[GROUP_MAX], nrc = 0, nrq = 0;
            bool big = false;
            for (uint32_t k = g; k < ge && !big; k++)
                if (p_match[p_order[k]] == -1) { if (nrc == GROUP_MAX) big = true; else rc[nrc++] = k - g; }
            for (uint32_t q = 0; q < nprev && !big; q++)
                if (!p_used[cand[q]]) { if (nrq == GROUP_MAX) big = true; else rq[nrq++] = q; }
            if (big) {
                for (uint32_t k = g; k < ge; k++) {
                    if (p_match[p_order[k]] != -1) continue;
                    int32_t pj = lookup_prev(key, cur->draws[p_order[k]].occ);
                    if (pj >= 0 && !p_used[pj]) p_match[p_order[k]] = pj, p_used[pj] = 1, st_occ_pairs++;
                }
            } else if (nrc && nrq) {
                uint32_t np = 0;
                for (uint32_t x = 0; x < nrc; x++)
                    for (uint32_t y = 0; y < nrq; y++)
                    {   // ties: the same pass of the object first (occurrence within obj + key), then the nearest occurrence
                        uint32_t c = rc[x], q = rq[y];
                        const DrawRec *dc = &cur->draws[p_order[g + c]], *dp = &prev->draws[cand[q]];
                        uint32_t dpass = dc->oocc > dp->oocc ? dc->oocc - dp->oocc : dp->oocc - dc->oocc;
                        pairs[np++] = (Pair){draw_distance(dc, dp), (uint16_t)c, (uint16_t)q,
                                             (uint16_t)((dpass > 255 ? 255 : dpass) << 8 | (c > q ? c - q : q - c) % 256)};
                    }
                qsort(pairs, np, sizeof *pairs, pair_cmp);
                for (uint32_t i = 0; i < np; i++) {
                    uint32_t j = p_order[g + pairs[i].c];
                    int32_t pj = cand[pairs[i].p];
                    if (p_match[j] != -1 || p_used[pj] || pairs[i].d >= 1e30f) continue;
                    if (draw_maxdiff(&cur->draws[j], &prev->draws[pj]) > cut_threshold()) { p_match[j] = -3; st_new_objects++; continue; }  // (another object)
                    p_match[j] = pj;
                    p_used[pj] = 1;
                    st_content_pairs++;
                }
            }
        }
        g = ge;
    }
}

// How far each logged copy moved from the frame before, by its offset (interp_frame_begin): a record whose draw had no
// partner in its frame has no motion of its own, and the jump rule compares with the frame before's motion - the
// camera's block, shared by every draw, takes it from the copy. (With the record's alone, every fast camera turn in the
// Cap Kingdom was a camera cut, 20 frames in a row: one draw with a new partner each frame was enough.)
#define MBITS 14
static void moves_put(Frame *f, uint32_t off, float maxd) {
    uint32_t h = (uint32_t)((off + 1) * 0x9E3779B1u) >> (32 - MBITS);
    for (int i = 0; i < 64; i++, h = (h + 1) & ((1 << MBITS) - 1)) {
        if (f->moves[h].off == off + 1) { if (maxd > f->moves[h].maxd) f->moves[h].maxd = maxd; return; }
        if (!f->moves[h].off) { f->moves[h].off = off + 1, f->moves[h].maxd = maxd; return; }
    }
}
static float moves_get(const Frame *f, uint32_t off) {
    if (!f->moves) return -1;
    uint32_t h = (uint32_t)((off + 1) * 0x9E3779B1u) >> (32 - MBITS);
    for (int i = 0; i < 64; i++, h = (h + 1) & ((1 << MBITS) - 1)) {
        if (f->moves[h].off == off + 1) return f->moves[h].maxd;
        if (!f->moves[h].off) return -1;
    }
    return -1;
}

// The shared blocks' motion by kind (stage, slot, size): a copy of one no draw of the frame before had a partner for
// (a pass's 256-byte camera block in a group of new draws) takes the motion of its kind's others.
static void cls_put(Frame *f, const CbRec *c, float maxd) {
    int k = 0;
    for (; k < f->ncls; k++) if (f->cls[k].stage == c->stage && f->cls[k].slot == c->slot && f->cls[k].size == c->size) break;
    if (k == f->ncls) {
        if (k == 16) return;
        f->cls[k].stage = c->stage, f->cls[k].slot = c->slot, f->cls[k].size = c->size, f->cls[k].maxd = maxd;
        f->ncls++;
    } else if (maxd > f->cls[k].maxd) f->cls[k].maxd = maxd;
}
static float cls_get(const Frame *f, const CbRec *c) {
    for (int k = 0; k < f->ncls; k++) if (f->cls[k].stage == c->stage && f->cls[k].slot == c->slot && f->cls[k].size == c->size) return f->cls[k].maxd;
    return -1;
}

static void frame_reset(Frame *f) {
    f->ndraws = f->ncbs = f->nhandles = f->nsto = f->ndout = 0;
    f->nbytes = 0;
    f->overflow = f->destroyed = false;
}

// ---- objects freed by the game's threads
static pthread_mutex_t free_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t free_gen;
void interp_free_begin(void) {
    if (pthread_mutex_trylock(&free_lock) != 0) {  // (a replay runs: the wait isn't the game's work - frame pacing)
        extern __thread uint64_t g_blocked_ns;
        uint64_t t0 = plat_now_ns();
        pthread_mutex_lock(&free_lock);
        g_blocked_ns += plat_now_ns() - t0;
    }
    atomic_fetch_add(&free_gen, 1);
}
void interp_free_end(void) { pthread_mutex_unlock(&free_lock); }
bool interp_replay_lock(void) {
    pthread_mutex_lock(&free_lock);
    if (atomic_load(&free_gen) == cur->free_gen) return true;
    pthread_mutex_unlock(&free_lock);
    return false;
}
void interp_replay_unlock(void) { pthread_mutex_unlock(&free_lock); }

// A buffer many draws bind (the camera's: 4800 bytes in each of Odyssey's ~1700 draws of a busy frame) is logged once
// per frame while its bytes stay the same: later records share the copy, and frame_end and interp_prepare compare and
// blend each shared copy once (their caches below) - per draw it was ~8 MB of element checks a frame with the camera
// moving: 12 ms, the game down to 36 fps.
#define DBITS 12
static struct { uint64_t addr; uint32_t size, off, gen; } dd[1 << DBITS];
static uint32_t dd_gen;

bool interp_list_submitted(uint64_t handle) {
    if (!g_interp_log || g_interp_replaying || !lg) return false;
    bool first = !frame_open;
    if (first) {
        frame_open = true;
        memset(occ_keys, 0, sizeof occ_keys);
        lg->free_gen = atomic_load(&free_gen);
        dd_gen++;
    }
    GROW(lg->handles, lg->nhandles, lg->cap_handles, 64);
    lg->handles[lg->nhandles++] = handle;
    return first;
}

// (the backend, at the frame's first list) its executor state: a replay of the frame starts from it
void interp_frame_state_save(const void *p, size_t n) {
    if (!frame_open || !lg) return;
    if (n > lg->state_cap) { lg->state_cap = n; lg->state = realloc(lg->state, n); }
    memcpy(lg->state, p, n);
    lg->state_n = n;
}
const void *interp_frame_state(size_t *n) {
    *n = cur ? cur->state_n : 0;
    return cur && cur->state_n ? cur->state : NULL;
}

void interp_texture_destroyed(void) { if (frame_open && lg) lg->destroyed = true; }

// ---- replay cursor
static uint32_t rp_draw, rp_cb;
static int32_t rp_cur = -1;  // the replayed draw's record (-1: not found: guest memory as it is)
static uint64_t st_replay_misses, st_replay_draws, st_replay_cb_miss;

bool interp_draw(uint64_t key, bool depth_only) {
    if (g_interp_replaying) {
        // A replayed draw finds its record among the next logged draws. One that has none was not in the frame (its
        // program was still being built then and the draw left out, ready now) - or not found: it read the uniform
        // buffers as the game has them by now, the next frame's camera already in them, and broke the in-between
        // (Tostarena: a parasol's colour pass at the next frame's camera, its depth pre-pass blended - the depth-equal
        // test failed, the parasol black in every in-between of a camera turn). Such a draw is left out (false).
        st_replay_draws++;
        rp_cur = -1;
        for (uint32_t j = rp_draw; j < cur->ndraws && j < rp_draw + 64; j++)
            if (cur->draws[j].key == key) { rp_cur = (int32_t)j; rp_draw = j + 1; rp_cb = cur->draws[j].first_cb; break; }
        if (rp_cur < 0) {
            st_replay_misses++;
            extern bool g_debug_frame;
            if (g_debug_frame) LOG("[interp] replay draw without its record: key %016llx (next record %u of %u)\n",
                                   (unsigned long long)key, rp_draw, cur->ndraws);
        }
        return rp_cur >= 0;
    }
    if (!frame_open || !lg) return true;  // (a list run outside a frame: nothing to track)
    GROW(lg->draws, lg->ndraws, lg->cap_draws, 1024);
    DrawRec *d = &lg->draws[lg->ndraws++];
    d->key = key;
    d->occ = next_occ(key);
    d->depth_only = depth_only;
    d->screen_pass = 0;
    d->first_cb = lg->ncbs;
    d->ncb = 0;
    return true;
}

const void *interp_cbuf(int stage, int k, int slot, const void *src, uint64_t size, uint64_t used) {
    (void)k;
    if (g_interp_replaying) {
        if (rp_cur < 0) return src;
        DrawRec *d = &cur->draws[rp_cur];
        if (rp_cb >= d->first_cb + d->ncb) { st_replay_cb_miss++; return src; }
        CbRec *c = &cur->cbs[rp_cb];
        if (c->stage != stage || c->slot != slot || c->size != size) { st_replay_cb_miss++; return src; }
        rp_cb++;
        extern bool g_debug_frame;
        static int dbg_cb = -1;  // ODYSSEY_INTERP_DEBUG_CB=<n> (research, a replay debug frame): each block's first n
        if (dbg_cb < 0) dbg_cb = getenv("ODYSSEY_INTERP_DEBUG_CB") ? atoi(getenv("ODYSSEY_INTERP_DEBUG_CB")) : 0;
        if (g_debug_frame && dbg_cb && stage <= 1) {  // values: blended (used), this frame's, the previous frame's
            const float *u = (const float *)(c->blend >= 0 ? blend_bytes + c->blend : cur->bytes + c->off),
                        *b = (const float *)(cur->bytes + c->off), *a = c->prev_off >= 0 ? (const float *)(prev->bytes + c->prev_off) : NULL;
            char line[4096];
            int o = snprintf(line, sizeof line, "[interp-cb] draw %d stage %d slot %d size %u snap %d prev %s:", rp_cur, stage, slot,
                             (unsigned)size, c->snap, a ? "yes" : "no");
            for (int q = 0; q < dbg_cb && q < (int)(c->used / 4) && o < (int)sizeof line - 64; q++)
                if (u[q] != b[q] || (a && a[q] != b[q])) o += snprintf(line + o, sizeof line - (size_t)o, " [%d] %g/%g/%g", q, u[q], b[q], a ? a[q] : 0.0f);
            LOG("%s\n", line);
        }
        return c->blend >= 0 ? blend_bytes + c->blend : cur->bytes + c->off;
    }
    if (!frame_open || !lg || !lg->ndraws) return src;
    DrawRec *d = &lg->draws[lg->ndraws - 1];
    if (lg->nbytes + size > (256u << 20)) { lg->overflow = true; return src; }
    while (lg->nbytes + size > lg->cap_bytes) {
        lg->cap_bytes = lg->cap_bytes ? lg->cap_bytes * 2 : (8u << 20);
        lg->bytes = realloc(lg->bytes, lg->cap_bytes);
    }
    GROW(lg->cbs, lg->ncbs, lg->cap_cbs, 4096);
    CbRec *c = &lg->cbs[lg->ncbs++];
    c->stage = (uint8_t)stage, c->slot = (uint8_t)slot, c->off = (uint32_t)lg->nbytes, c->size = (uint32_t)size;
    c->used = stage >= 2 ? 0 : (uint32_t)(used && used < size ? used : size);  // (stage 2, 3: a texture entry - never compared)
    c->addr = (uint64_t)(uintptr_t)src;
    c->prev_off = c->blend = c->prev2_off = -1;
    c->snap = 0, c->maxd = -1;
    d->ncb++;
    uint32_t h = (uint32_t)(((uint64_t)(uintptr_t)src * 0x9E3779B97F4A7C15ull ^ size) >> (64 - DBITS));
    if (dd[h].gen == dd_gen && dd[h].addr == (uint64_t)(uintptr_t)src && dd[h].size == size &&
        !memcmp(lg->bytes + dd[h].off, src, size)) {
        c->off = dd[h].off;  // (the same bytes as logged for an earlier draw: shared)
        return src;
    }
    dd[h].gen = dd_gen, dd[h].addr = (uint64_t)(uintptr_t)src, dd[h].size = (uint32_t)size, dd[h].off = c->off;
    memcpy(lg->bytes + lg->nbytes, src, size);
    lg->nbytes += (size + 15) & ~15ull;
    return src;
}

// Storage read through gmem (the light lists, a per-frame table): its bytes at execution time, once per range per
// frame (64 KB at most each), and the frame's dispatch outputs. Replays: interp_replays_allowed compares.
void interp_storage(uint64_t addr, uint64_t size) {
    if (g_interp_replaying || !frame_open || !lg || !addr || !size) return;
    if (size > 0x10000) size = 0x10000;
    for (uint32_t i = 0; i < lg->nsto; i++) if (lg->sto[i].addr == addr) return;
    if (lg->nbytes + size > (256u << 20)) { lg->overflow = true; return; }
    while (lg->nbytes + size > lg->cap_bytes) { lg->cap_bytes = lg->cap_bytes ? lg->cap_bytes * 2 : (8u << 20); lg->bytes = realloc(lg->bytes, lg->cap_bytes); }
    GROW(lg->sto, lg->nsto, lg->cap_sto, 64);
    lg->sto[lg->nsto++] = (StoRec){addr, size, lg->nbytes};
    memcpy(lg->bytes + lg->nbytes, (const void *)(uintptr_t)addr, size);
    lg->nbytes += (size + 15) & ~15ull;
}
void interp_dispatch_output(uint64_t addr, uint64_t size) {
    if (g_interp_replaying || !frame_open || !lg || !addr) return;
    GROW(lg->dout, lg->ndout, lg->cap_dout, 64);
    lg->dout[lg->ndout++].addr = addr, lg->dout[lg->ndout - 1].size = size;
}

// Texture-pool entries: logged like uniform blocks (stage 2 + the stage, slot i, no bytes compared: used = 0) and
// read back from the log by a replay, in the draw's bind order after its uniform blocks.
const void *interp_texentry(int stage, int i, const void *entry, uint32_t size) {
    const void *r = interp_cbuf(2 + stage, 0, i, entry, size, 0);
    return r;
}

// ---- blending
static bool blendable(float a, float b) {
    if (!isfinite(a) || !isfinite(b) || fabsf(a) > 1e6f || fabsf(b) > 1e6f) return false;
    if ((a != 0 && fabsf(a) < 1e-30f) || (b != 0 && fabsf(b) < 1e-30f)) return false;  // (integers: denormals)
    return true;
}
static float cut_threshold(void) {
    static float c = -1;
    // (Odyssey's world is in centimetres: Mario runs ~14 a frame, the camera orbits ~1000-2000 away - its view matrix's
    // translation moves ~130 a frame as it starts turning; a warp moves thousands. LA's 4 made every turn a cut.)
    if (c < 0) { const char *e = getenv("ODYSSEY_INTERP_CUT"); c = e ? (float)atof(e) : 300.0f; }
    return c;
}
static int blend_stages(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("ODYSSEY_INTERP_STAGES"); m = e ? atoi(e) : 3; }
    return m;
}

// ---- stats (per log period)
static uint64_t s_frames, s_draws, s_matched, s_changed, s_cut_cbs, s_blended_cbs, s_cuts[9], s_frame_no;
static double s_replay_cpu, s_replay_cpu_max, s_prep;
static uint64_t s_replays;
static const char *const cut_name[9] = {"level/forced", "no previous frame", "few matches", "jumps", "object freed", "loading/hitch", "camera jump", "game ahead", "storage rewritten"};

static void log_cut(int why, const char *more) {
    s_cuts[why]++;
    static int on = -1;
    if (on < 0) on = getenv("ODYSSEY_INTERP_LOG_CUTS") != NULL;
    extern const char *game_level_name(void);
    if (on) LOG("[interp-cut] f%llu %s: %s%s\n", (unsigned long long)s_frame_no, game_level_name(), cut_name[why], more ? more : "");
}

void interp_force_cut(void) { force_cut = true; history_reset = true; }

// The game's uniform blocks are double-buffered by frame parity: once it is two frames past the frame being replayed it
// rewrites that frame's memory, and a replay's vertex data, index data and texture uploads (read live, unlike the logged
// uniforms) tear.
bool interp_replays_allowed(uint64_t frame, uint64_t game_frame) {
    if (game_frame >= frame + 2) { log_cut(7, NULL); return false; }
    // Storage the frame's gmem draws read, rewritten by the game since (its CPU-written tables; a dispatch's output
    // is left to GPU order): the replays would read the newer data - the village's lighting one image different.
    for (uint32_t i = 0; i < cur->nsto; i++) {
        StoRec *r = &cur->sto[i];
        bool gpu = false;
        for (uint32_t k = 0; k < cur->ndout && !gpu; k++) gpu = r->addr < cur->dout[k].addr + cur->dout[k].size && cur->dout[k].addr < r->addr + r->size;
        if (gpu) continue;
        if (memcmp(cur->bytes + r->off, (const void *)(uintptr_t)r->addr, r->size) != 0) {
            char why[80];
            snprintf(why, sizeof why, " (%llx, %llu bytes)", (unsigned long long)r->addr, (unsigned long long)r->size);
            log_cut(8, why);
            return false;
        }
    }
    return true;
}

// ODYSSEY_INTERP_DUMP=<frame>:<path> (research): both frames' draws and uniform copies (with their guest addresses) as
// text at present frames frame .. frame + 2 (<path>.0 .. .2)
static void dump_frames(void) {
    static long df = -1; static char dpath[256];
    if (df < 0) { const char *e = getenv("ODYSSEY_INTERP_DUMP"); df = 0; if (e) sscanf(e, "%ld:%255s", &df, dpath); }
    if (df && (long)g_interp_present_frame >= df && (long)g_interp_present_frame < df + 3) {  // (3 frames: N-1 .. N+2)
        char path[300];
        snprintf(path, sizeof path, "%s.%ld", dpath, (long)g_interp_present_frame - df);
        FILE *f = fopen(path, "w");
        for (int which = 0; f && which < 2; which++) {
            Frame *F = which ? cur : prev;
            fprintf(f, "F %s %u\n", which ? "cur" : "prev", F->ndraws);
            for (uint32_t j = 0; j < F->ndraws; j++) {
                DrawRec *d = &F->draws[j];
                fprintf(f, "D %u %016llx %u %u\n", j, (unsigned long long)d->key, d->occ, d->ncb);
                for (uint32_t i = 0; i < d->ncb; i++) {
                    CbRec *c = &F->cbs[d->first_cb + i];
                    fprintf(f, "C %d %d %u %llx", c->stage, c->slot, c->size, (unsigned long long)c->addr);
                    const uint32_t *w = (const uint32_t *)(F->bytes + c->off);
                    for (uint32_t q = 0; q < c->size / 4; q++) fprintf(f, " %08x", w[q]);
                    fprintf(f, "\n");
                }
            }
        }
        if (f) fclose(f);
    }
}

// Comparison results per pair of copies (previous frame's, this frame's, bytes compared), for one frame_end; and the
// blended copies per (copies, snap) for one interp_prepare: shared copies are worked on once.
#define CBITS 14
typedef struct { uint32_t poff, coff, used, gen; bool valid, same, far; uint32_t jq; float maxd; } CmpRes;
static CmpRes cmp_cache[1 << CBITS];
static uint32_t cmp_gen;
static CmpRes *cmp_find(uint32_t poff, uint32_t coff, uint32_t used) {
    uint64_t k = ((uint64_t)poff << 32 | coff) ^ (uint64_t)used * 0x9E3779B97F4A7C15ull;
    CmpRes *r = &cmp_cache[(uint32_t)((k * 0xff51afd7ed558ccdull) >> (64 - CBITS))];
    if (r->gen != cmp_gen || r->poff != poff || r->coff != coff || r->used != used) {
        r->gen = cmp_gen, r->poff = poff, r->coff = coff, r->used = used, r->valid = false;
    }
    return r;
}
typedef struct { int64_t prev_off, prev2_off; uint32_t off, gen, used; uint8_t snap; int64_t blend; } BlendRes;
static BlendRes blend_cache[1 << CBITS];
static uint32_t blend_gen;

// (game thread, at its present) the logged frame is closed: returned for the interpolation thread (interp_frame_begin,
// or interp_frame_drop), NULL when nothing was logged; the next frame is logged when log_next.
InterpFrame *interp_frame_close(bool log_next, uint64_t frame_no) {
    bool was_open = frame_open, was = g_interp_log;
    frame_open = false;
    g_interp_log = log_next;
    if (!lg) {  // (no free slot at the last close: the interpolation thread is behind)
        pthread_mutex_lock(&fr_lock);
        for (int i = 0; i < NFR && !lg; i++) if (!fr[i].busy) { lg = &fr[i]; lg->busy = true; }
        pthread_mutex_unlock(&fr_lock);
        if (lg) frame_reset(lg);
        else g_interp_log = false;
        return NULL;
    }
    if (!was || !was_open || !lg->ndraws || lg->overflow) { frame_reset(lg); return NULL; }
    Frame *done = lg;
    done->frame_no = frame_no;
    lg = NULL;
    pthread_mutex_lock(&fr_lock);
    for (int i = 0; i < NFR && !lg; i++) if (!fr[i].busy) { lg = &fr[i]; lg->busy = true; }
    pthread_mutex_unlock(&fr_lock);
    if (lg) frame_reset(lg);
    else g_interp_log = false;
    return (InterpFrame *)done;
}

// (interpolation thread) a closed frame it won't take: its slot back
void interp_frame_drop(InterpFrame *f) {
    if (!f) return;
    pthread_mutex_lock(&fr_lock);
    ((Frame *)f)->busy = false;
    pthread_mutex_unlock(&fr_lock);
}

// (interpolation thread) the closed frame against the two before it: whether in-betweens can be made. Then
// interp_prepare / the replays, then interp_frame_rotate (always: it becomes the previous frame).
bool interp_frame_begin(InterpFrame *f) {
    double t0 = plat_now_ns() / 1e6;
    cmp_gen++;
    s_frame_no++;
    can_blend = false;
    cur = (Frame *)f;
    if (!cur) return false;
    for (uint32_t j = 0; j < cur->ndraws; j++) cur->draws[j].pair = -1;  // (set by match_all when the frame is paired)
    if (!cur->moves) cur->moves = malloc(sizeof *cur->moves << MBITS);
    memset(cur->moves, 0, sizeof *cur->moves << MBITS);
    cur->ncls = 0;
    s_frames++;
    if (force_cut) { force_cut = false; log_cut(0, NULL); return false; }
    if (!prev_valid || prev->overflow) { log_cut(1, NULL); return false; }
    if (prev->frame_no + 1 != cur->frame_no) { log_cut(1, " (a frame between them was not logged)"); history_reset = true; return false; }
    index_prev();
    assign_objects(cur);
    dump_frames();
    match_all();
    for (uint32_t j = 0; j < cur->ndraws; j++) cur->draws[j].pair = p_match[j] >= 0 ? p_match[j] : -1;
    history_reset = false;  // (this frame paired without the old chain: from here on it is the chain)
    s_id_pairs += st_id_pairs, s_content_pairs += st_content_pairs, s_new_objects += st_new_objects, s_same_pairs += st_same_pairs, s_occ_pairs += st_occ_pairs;
    float cut = cut_threshold();
    int stages = blend_stages();
    uint64_t matched = 0, changed = 0, cuts = 0;
    for (uint32_t j = 0; j < cur->ndraws; j++) {
        DrawRec *d = &cur->draws[j];
        int32_t pj = p_match[j];
        if (pj == -2 || pj == -3) { matched++; continue; }  // (UI, or an object that just appeared: shown as it is)
        if (pj < 0) continue;
        matched++;
        DrawRec *p = &prev->draws[pj];
        for (uint32_t i = 0; i < d->ncb && i < p->ncb; i++) {
            CbRec *cc = &cur->cbs[d->first_cb + i], *pc = &prev->cbs[p->first_cb + i];
            if (cc->stage != pc->stage || cc->slot != pc->slot || cc->size != pc->size) continue;
            if (cc->stage > 1 || !(stages & (cc->stage == 0 ? 1 : 2))) continue;  // (texture entries are never blended)
            const float *a = (const float *)(prev->bytes + pc->off), *b = (const float *)(cur->bytes + cc->off);
            // (a pair of shared copies - the camera's block, bound by every draw - is compared once: cmp_cache)
            CmpRes *cr = cmp_find(pc->off, cc->off, cc->used);
            if (!cr->valid) {
                cr->valid = true;
                cr->same = !memcmp(a, b, cc->used);
                uint32_t nf = cc->used / 4;
                // A jump: an element moved further than the jump distance in one frame - and much further than the
                // buffer moved the frame before. Steady fast motion (a dungeon's room-to-room scroll: its camera moves
                // far each frame, every frame) blends; deciding it per frame by distance alone made such a scroll
                // alternate between blended and snapped in-betweens: flicker.
                cr->maxd = 0, cr->jq = 0, cr->far = false;
                for (uint32_t z = 0; !cr->same && z < nf; z++) {
                    if (!blendable(a[z], b[z])) continue;
                    float d = fabsf(b[z] - a[z]);
                    if (d > cr->maxd) cr->maxd = d;
                    if (!cr->far && d > cut + 0.05f * fmaxf(fabsf(a[z]), fabsf(b[z]))) cr->far = true, cr->jq = z + 1;
                }
            }
            if (cr->same) {
                cc->maxd = 0;
                moves_put(cur, cc->off, 0);
                if (cc->slot <= 1) cls_put(cur, cc, 0);
                continue;
            }
            changed++;
            uint32_t nf = cc->used / 4;
            float maxd = cr->maxd;
            uint32_t q = nf, jq = cr->jq;
            bool far = cr->far;
            cc->maxd = maxd;
            moves_put(cur, cc->off, maxd);
            if (cc->slot <= 1) cls_put(cur, cc, maxd);
            {   // ODYSSEY_INTERP_LOG_MOVES=<d>: buffers whose elements moved more than d in one frame (research:
                // draws matched to another object of the same mesh)
                static float lm = -2; static int nlm; static long lmf;  // ODYSSEY_INTERP_LOG_MOVES_FROM=<frame>
                if (lm < -1) { const char *e = getenv("ODYSSEY_INTERP_LOG_MOVES"); lm = e ? (float)atof(e) : -1;
                               e = getenv("ODYSSEY_INTERP_LOG_MOVES_FROM"); lmf = e ? atol(e) : 0; }
                if (lm >= 0 && maxd > lm && (long)s_frame_no >= lmf && nlm++ < 40000) {
                    uint32_t mq = 0; for (uint32_t z = 0; z < nf; z++) if (blendable(a[z], b[z]) && fabsf(b[z] - a[z]) == maxd) { mq = z; break; }
                    LOG("[interp-move] f%llu draw %u occ %u key %016llx stage %d slot %d size %u ncb %u: maxd %g at elem %u (%g -> %g) far %d\n",
                        (unsigned long long)s_frame_no, j, d->occ, (unsigned long long)d->key, cc->stage, cc->slot, cc->size, d->ncb, maxd, mq, a[mq], b[mq], far);
                }
            }
            // (the shared blocks - slots 0, 1: the camera's, a pass's - three times the distance: a camera starting to
            // turn far from the world's origin moves its view's translation by that distance times the angle, ~460
            // units in the frame it started in the Cap Kingdom)
            float pm = pc->maxd >= 0 ? pc->maxd : moves_get(prev, pc->off), cj = cc->slot <= 1 ? 3 * cut : cut;
            if (pm < 0 && cc->slot <= 1) pm = cls_get(prev, pc);
            bool jump = far && maxd > cj && (pm < 0 || maxd > 2.5f * pm + cj);
            q = jq;
            cc->snap = jump;
            if (jump) {
                static int nlog;  // ODYSSEY_INTERP_LOG_JUMPS=<frame>: log jumping buffers from that frame on
                static long from = -1;
                if (from < 0) from = getenv("ODYSSEY_INTERP_LOG_JUMPS") ? atol(getenv("ODYSSEY_INTERP_LOG_JUMPS")) : 0;
                if (from && (long)s_frame_no >= from && nlog++ < 3000)
                    LOG("[interp-jump] f%llu draw %u stage %d slot %d size %u elem %u: %g -> %g (moved %g, the frame before %g)\n",
                        (unsigned long long)s_frame_no, j, cc->stage, cc->slot, cc->size, q - 1, a[q - 1], b[q - 1], maxd, pm);
                cuts++;
            }
            cc->prev_off = pc->off;
            {   // the same buffer two frames back, through the partner's own partner (the toggle rule, interp_prepare)
                int32_t y = p->pair;
                if (y >= 0 && (uint32_t)y < prev2->ndraws && i < prev2->draws[y].ncb) {
                    CbRec *p2 = &prev2->cbs[prev2->draws[y].first_cb + i];
                    if (p2->stage == cc->stage && p2->slot == cc->slot && p2->size == cc->size) cc->prev2_off = p2->off;
                }
            }
        }
    }
    {   // One decision per object: a draw with a per-object block that jumped shows this frame's values in all of its
        // per-object blocks, both stages - and so does every draw sharing one of those blocks (the object's other
        // passes). Decided per buffer, Tostarena's shop keeper's shadow volume took this frame's fragment values (its
        // reach, 8550 -> 0) and blended its vertex block (the box): a box half-way between parked and in place, a huge
        // dark silhouette on the sand for an in-between every third frame - the flicker while the camera moved.
        enum { SBITS = 14 };
        static uint32_t snapped[1 << SBITS];  // (open hash of the copies' offsets + 1)
        memset(snapped, 0, sizeof snapped);
        uint32_t nsn = 0;
        uses_build();
        // Draws without a partner (an object that just appeared, one whose partner was too far: -3, -1) show their own
        // blocks as this frame has them - but in the in-between's camera and pass: their shared blocks blend as the
        // other draws' copies of them do. And the object's other draws (its depth pre-pass) take this frame's values
        // for the blocks they share with it. Before, such a draw kept this frame's camera while the object's pre-pass
        // blended: its colour pass (depth equal) failed everywhere - in Tostarena a parasol near the camera turned
        // black in every in-between of a camera turn, the screen flickering.
        // (By content: the depth pre-pass reads the camera from a 256-byte block of its own, the colour pass from the
        // scene's 4800-byte one - Tostarena's parasol, partnerless in both passes, got the blended camera in its colour
        // pass and this frame's in its pre-pass, the block's copy being bound by no partnered draw: black. A shared
        // block takes the partner of any partnered draw's block with the same bytes.)
        {
            static struct { uint64_t key; int64_t prev_off, prev2_off; } smap[1 << SBITS];
            static struct { uint32_t off; uint64_t key; } khash[1 << SBITS];  // (each copy's content key, once)
            memset(smap, 0, sizeof smap);
            memset(khash, 0, sizeof khash);
            #define CB_KEY(cc, out) do { \
                uint32_t kh_ = ((cc)->off * 0x9E3779B1u) >> (32 - SBITS); int kk_ = 0; \
                while (kk_++ < 64 && khash[kh_].off && khash[kh_].off != (cc)->off + 1) kh_ = (kh_ + 1) & ((1 << SBITS) - 1); \
                if (khash[kh_].off == (cc)->off + 1) (out) = khash[kh_].key; \
                else { \
                    uint64_t h_ = 0xcbf29ce484222325ull ^ ((uint64_t)(cc)->stage << 48 | (uint64_t)(cc)->slot << 40 | (cc)->size); \
                    const uint8_t *p_ = cur->bytes + (cc)->off; \
                    for (uint32_t q_ = 0; q_ + 8 <= (cc)->used; q_ += 8) { uint64_t w_; memcpy(&w_, p_ + q_, 8); h_ = (h_ ^ w_) * 0x100000001b3ull; } \
                    (out) = h_ | 1; \
                    if (!khash[kh_].off) khash[kh_].off = (cc)->off + 1, khash[kh_].key = (out); \
                } } while (0)
            for (uint32_t j = 0; j < cur->ndraws; j++) {
                if (p_match[j] < 0) continue;
                DrawRec *d = &cur->draws[j];
                for (uint32_t i = 0; i < d->ncb; i++) {
                    CbRec *cc = &cur->cbs[d->first_cb + i];
                    if (cc->stage > 1 || cc->slot > 1 || cc->prev_off < 0) continue;
                    uint64_t key;
                    CB_KEY(cc, key);
                    uint32_t h = (uint32_t)(key >> (64 - SBITS));
                    for (int k = 0; k < 64 && smap[h].key && smap[h].key != key; k++) h = (h + 1) & ((1 << SBITS) - 1);
                    if (!smap[h].key) smap[h].key = key, smap[h].prev_off = cc->prev_off, smap[h].prev2_off = cc->prev2_off;
                }
            }
            for (uint32_t j = 0; j < cur->ndraws; j++) {
                if (p_match[j] != -1 && p_match[j] != -3) continue;
                DrawRec *d = &cur->draws[j];
                for (uint32_t i = 0; i < d->ncb; i++) {
                    CbRec *cc = &cur->cbs[d->first_cb + i];
                    if (cc->stage > 1) continue;
                    uint32_t h = (cc->off * 0x9E3779B1u) >> (32 - SBITS);
                    if (cc->slot <= 1) {
                        uint64_t key;
                        CB_KEY(cc, key);
                        for (uint32_t s = (uint32_t)(key >> (64 - SBITS)), k = 0; k < 64 && smap[s].key; k++, s = (s + 1) & ((1 << SBITS) - 1))
                            if (smap[s].key == key) { cc->prev_off = smap[s].prev_off, cc->prev2_off = smap[s].prev2_off, cc->snap = 0; break; }
                    } else if (nsn < (1u << SBITS) / 2 && few_uses(cc)) {
                        while (snapped[h] && snapped[h] != cc->off + 1) h = (h + 1) & ((1 << SBITS) - 1);
                        if (!snapped[h]) snapped[h] = cc->off + 1, nsn++;
                    }
                }
            }
        }
        #undef CB_KEY
        for (int round = 0; round < 3; round++) {
            bool grew = false;
            for (uint32_t j = 0; j < cur->ndraws; j++) {
                DrawRec *d = &cur->draws[j];
                bool any = false;
                for (uint32_t i = 0; i < d->ncb && !any; i++) {
                    CbRec *cc = &cur->cbs[d->first_cb + i];
                    if (cc->stage > 1 || cc->slot < 2 || cc->prev_off < 0) continue;
                    if (cc->snap) { any = true; break; }
                    if (nsn)
                        for (uint32_t h = (cc->off * 0x9E3779B1u) >> (32 - SBITS), k = 0; snapped[h] && k < 64; h = (h + 1) & ((1 << SBITS) - 1), k++)
                            if (snapped[h] == cc->off + 1) { any = true; break; }
                }
                if (!any) continue;
                for (uint32_t i = 0; i < d->ncb; i++) {
                    CbRec *cc = &cur->cbs[d->first_cb + i];
                    if (cc->stage > 1 || cc->slot < 2 || cc->prev_off < 0) continue;
                    if (!cc->snap) cc->snap = 1, s_class_snaps++, grew = true;
                    if (nsn >= (1u << SBITS) / 2 || !few_uses(cc)) continue;
                    uint32_t h = (cc->off * 0x9E3779B1u) >> (32 - SBITS);
                    while (snapped[h] && snapped[h] != cc->off + 1) h = (h + 1) & ((1 << SBITS) - 1);
                    if (!snapped[h]) snapped[h] = cc->off + 1, nsn++, grew = true;
                }
            }
            if (!grew) break;
        }
    }
    {   // One decision per light (Dusklight: the light's view and projection are recorded once and every reader -
        // the caster pass and the receivers' texture matrix - takes the same replacement, src/d/d_drawlist.cpp).
        // The light's block is the shared block (slots 0 and 1) the shadow pass binds; the ground reads a copy of it
        // in the fragment stage. Decided per buffer, one copy could snap while the other blended: the shadow map
        // drawn with one light matrix and read with another (acne, a wedge of shadow). A jump in any copy snaps
        // them all - and only them: the earlier rule took every block of the same slot and size with it, the
        // scene's camera included, so a light's jump put the camera at this frame while the objects blended: the
        // whole image shifted for one in-between (the full-screen flicker). A jump in any other shared block - the
        // camera, a pass - is a camera cut: the frame shows as it is, with no in-betweens (Dusklight's presentation
        // sync: the game marks its camera cuts and the frame is presented at step 1).
        struct { uint8_t slot; uint32_t size; bool snap; } lcls[32];
        int nl = 0;
        bool camera_jump = false;
        for (uint32_t j = 0; j < cur->ndraws; j++) {
            DrawRec *d = &cur->draws[j];
            if (!d->depth_only) continue;
            for (uint32_t i = 0; i < d->ncb; i++) {
                CbRec *cc = &cur->cbs[d->first_cb + i];
                if (cc->stage > 1 || cc->slot > 1 || cc->prev_off < 0) continue;
                int k = 0;
                for (; k < nl; k++) if (lcls[k].slot == cc->slot && lcls[k].size == cc->size) break;
                if (k == nl) { if (nl == 32) continue; lcls[nl].slot = cc->slot, lcls[nl].size = cc->size, lcls[nl].snap = false; nl++; }
                lcls[k].snap |= cc->snap;
            }
        }
        for (uint32_t j = 0; j < cur->ndraws; j++) {  // the receivers: the same block in the fragment stage
            DrawRec *d = &cur->draws[j];
            if (d->depth_only) continue;
            for (uint32_t i = 0; i < d->ncb; i++) {
                CbRec *cc = &cur->cbs[d->first_cb + i];
                if (cc->slot > 1 || cc->prev_off < 0 || cc->stage != 1) continue;
                for (int k = 0; k < nl; k++) if (lcls[k].slot == cc->slot && lcls[k].size == cc->size) { lcls[k].snap |= cc->snap; break; }
            }
        }
        for (uint32_t j = 0; j < cur->ndraws; j++) {
            DrawRec *d = &cur->draws[j];
            for (uint32_t i = 0; i < d->ncb; i++) {
                CbRec *cc = &cur->cbs[d->first_cb + i];
                if (cc->stage > 1 || cc->slot > 1 || cc->prev_off < 0) continue;
                int k = 0;
                bool light = d->depth_only || cc->stage == 1;
                if (light) { for (; k < nl; k++) if (lcls[k].slot == cc->slot && lcls[k].size == cc->size) break; light = k < nl; }
                if (light) { if (lcls[k].snap && !cc->snap) cc->snap = 1, s_class_snaps++; }
                else if (cc->snap && !d->screen_pass) camera_jump = true;  // (a pass's own jump: that pass shows this frame's)
            }
        }
        if (camera_jump) { log_cut(6, NULL); return false; }
    }
    s_draws += cur->ndraws, s_matched += matched, s_changed += changed, s_cut_cbs += cuts, s_blended_cbs += changed - cuts;
    s_prep += plat_now_ns() / 1e6 - t0;
    char why[96];
    if (matched < cur->ndraws * 8 / 10) {
        snprintf(why, sizeof why, " (%llu of %u draws)", (unsigned long long)matched, cur->ndraws);
        log_cut(2, why);
        return false;
    }
    if (cuts > 8 && cuts * 2 > changed) {  // (most of what moved jumped: a camera cut, a warp)
        snprintf(why, sizeof why, " (%llu of %llu changed buffers)", (unsigned long long)cuts, (unsigned long long)changed);
        log_cut(3, why);
        return false;
    }
    if (cur->destroyed) { log_cut(4, " (a texture)"); return false; }
    can_blend = true;
    return true;
}

bool interp_prepare(float t) {
    if (!can_blend) return false;
    double t0 = plat_now_ns() / 1e6;
    blend_n = 0;
    blend_gen++;
    for (uint32_t i = 0; i < cur->ncbs; i++) {
        CbRec *cc = &cur->cbs[i];
        cc->blend = -1;
        if (cc->prev_off < 0) continue;
        {   // (the same copies blended for an earlier record - a shared block: that blend)
            uint64_t k = ((uint64_t)cc->off << 32 ^ (uint64_t)cc->prev_off) * 0x9E3779B97F4A7C15ull ^ (uint64_t)(cc->prev2_off + 1) ^
                         (uint64_t)cc->used << 40 ^ cc->snap;
            BlendRes *br = &blend_cache[(uint32_t)((k * 0xff51afd7ed558ccdull) >> (64 - CBITS))];
            if (br->gen == blend_gen && br->off == cc->off && br->prev_off == cc->prev_off && br->prev2_off == cc->prev2_off &&
                br->used == cc->used && br->snap == cc->snap) {
                cc->blend = br->blend;
                continue;
            }
            br->gen = blend_gen, br->off = cc->off, br->prev_off = cc->prev_off, br->prev2_off = cc->prev2_off, br->used = cc->used;
            br->snap = cc->snap;
            br->blend = (int64_t)blend_n;  // (the blend below lands there)
        }
        while (blend_n + cc->size + 16 > blend_cap) {
            blend_cap = blend_cap ? blend_cap * 2 : (4u << 20);
            blend_bytes = realloc(blend_bytes, blend_cap);
        }
        const float *a = (const float *)(prev->bytes + cc->prev_off), *b = (const float *)(cur->bytes + cc->off);
        float *out = (float *)(blend_bytes + blend_n);
        uint32_t nf = cc->used / 4;
        // A buffer that jumped shows this frame's values whole (letsgo-recomp's rule: a matrix with some elements
        // snapped and others blended shears and shrinks the model; the earlier per-element snap did that).
        // ODYSSEY_INTERP_SNAP=elem: the old per-element snap (research).
        static int snap_elem = -1, toggles = -1;
        if (snap_elem < 0) {
            const char *e = getenv("ODYSSEY_INTERP_SNAP"); snap_elem = e && !strcmp(e, "elem");
            e = getenv("ODYSSEY_INTERP_TOGGLE"); toggles = !e || atoi(e);  // =0: blend toggling values too (research)
        }
        if (cc->snap && !snap_elem) { memcpy(out, b, cc->size); cc->blend = (int64_t)blend_n; blend_n += (cc->size + 15) & ~15u; continue; }
        float cut = cut_threshold();
        // An element back at its value of two frames ago alternates (dither offsets, frame parity, a jitter) - it is
        // not moving: it keeps this frame's value instead of a midpoint neither frame shows.
        const float *a2 = cc->prev2_off >= 0 ? (const float *)(prev2->bytes + cc->prev2_off) : NULL;
        for (uint32_t q = 0; q < nf; q++) {
            float av = a[q], bv = b[q], d = bv - av;
            bool toggle = toggles && a2 && a2[q] == bv && av != bv;
            s_toggled += toggle;
            if (!blendable(av, bv) || toggle || (cc->snap && fabsf(d) > cut + 0.05f * fmaxf(fabsf(av), fabsf(bv)))) { out[q] = bv; continue; }
            // Values that are whole numbers in both frames and changed by one or more are a selection, not motion
            // (a texture's frame, a mode): this frame's - unless the value took the same step the frame before (a
            // frame counter, 1127 -> 1128 every frame: time, blended). Dusklight never blends a texture-swap
            // animation (src/dusk/interp/material.cpp: a texNo track that changed is left at the sim frame's value).
            static long log_rules = -1;  // ODYSSEY_INTERP_LOG_RULES=<frame> (research): the rules' decisions for 5 frames from there
            if (log_rules < 0) { const char *e = getenv("ODYSSEY_INTERP_LOG_RULES"); log_rules = e ? atol(e) : 0; }
            bool lr = log_rules && (long)s_frame_no >= log_rules && (long)s_frame_no < log_rules + 5;
            if (av == rintf(av) && bv == rintf(bv) && fabsf(d) >= 1.0f && (!a2 || (a2[q] == rintf(a2[q]) && av - a2[q] != d))) {
                if (lr) LOG("[interp-rule] f%llu whole-number cb %u stage %d slot %d elem %u: %g, %g -> %g\n", (unsigned long long)s_frame_no, i, cc->stage, cc->slot, q, a2 ? a2[q] : NAN, av, bv);
                out[q] = bv; s_integral++; continue;
            }
            // A texture translation that scrolls a little every frame and wraps (0.98 -> 0.01) changed by nearly a
            // whole tile: blended, the wrap's midpoint is on screen for an image (the water's ripples jumping back).
            // When the change less a whole number of tiles is the step the value took the frame before, it is a
            // wrap: blend along the short way, which texture addressing wraps back (Dusklight's
            // sample_texture_translation, src/dusk/interp/material.cpp: a whole-tile jump between adjacent keys
            // continues the translation linearly).
            // (only values that look like texture coordinates: inside (-1, 1) and not a whole number at either end -
            // a lighting weight that faded 0.1, 0.05 and then jumped to 1 is no wrap, though its wrapped step matches)
            if (a2 && fabsf(d) >= 0.9f && fabsf(av) < 1 && fabsf(bv) < 1 && av != rintf(av) && bv != rintf(bv)) {
                float k = rintf(d), dw = d - k, s = av - a2[q];
                if (k != 0 && dw != 0 && fabsf(dw) < 0.5f && fabsf(s - dw) <= 0.1f * fabsf(dw) + 1e-5f) {
                    if (lr) LOG("[interp-rule] f%llu wrap cb %u stage %d slot %d elem %u: %g, %g -> %g\n", (unsigned long long)s_frame_no, i, cc->stage, cc->slot, q, a2[q], av, bv);
                    out[q] = av + dw * t; s_wraps++; continue;
                }
            }
            // A discontinuity: a change over half the value's size and over eight times the step it took the frame
            // before (a lighting weight jumping 0.05 -> 1, Link's facing turning in one frame) is a switch, not motion:
            // this frame's value, as Dusklight shows an animation that was not continuous this tick at its current
            // value (src/dusk/interp/material.cpp, Update::~Update's `continuous`). Blended, the in-between showed a
            // state neither frame has (the village 5% darker for one image).
            if (a2 && fabsf(d) > 0.5f * fmaxf(fabsf(av), fabsf(bv)) && fabsf(d) > 8.0f * fabsf(av - a2[q]) + 1e-4f) {
                if (lr) LOG("[interp-rule] f%llu discontinuity cb %u stage %d slot %d elem %u: %g, %g -> %g\n", (unsigned long long)s_frame_no, i, cc->stage, cc->slot, q, a2[q], av, bv);
                out[q] = bv; s_discont++; continue;
            }
            out[q] = av + d * t;
        }
        memcpy((uint8_t *)out + nf * 4, (const uint8_t *)b + nf * 4, cc->size - nf * 4);
        cc->blend = (int64_t)blend_n;
        blend_n += (cc->size + 15) & ~15u;
    }
    s_prep += plat_now_ns() / 1e6 - t0;
    return true;
}

uint32_t interp_handles(const uint64_t **out) { *out = cur->handles; return cur->nhandles; }

// ---- replays
static uint64_t rp_t0;
void interp_replay_begin(void) {
    g_interp_replaying = true;
    rp_draw = rp_cb = 0;
    rp_cur = -1;
    rp_t0 = plat_now_ns();
}

void interp_replay_end(void) {
    if (!g_interp_replaying) return;
    g_interp_replaying = false;
    uint64_t d = plat_now_ns() - rp_t0;
    s_replays++;
    s_replay_cpu += d / 1e6;
    if (d / 1e6 > s_replay_cpu_max) s_replay_cpu_max = d / 1e6;
}

void interp_frame_rotate(void) {
    if (!cur) return;
    pthread_mutex_lock(&fr_lock);
    prev2->busy = false;  // (the oldest goes back to the free slots)
    pthread_mutex_unlock(&fr_lock);
    prev2 = prev; prev = cur; cur = NULL;
    prev_valid = prev->ndraws > 0 && !prev->overflow;
    for (uint32_t i = 0; i < prev->ncbs; i++) prev->cbs[i].prev_off = prev->cbs[i].blend = -1;
}

void interp_log_stats(void) {
    static double every = -1, last;
    if (every < 0) { const char *e = getenv("ODYSSEY_INTERP_LOG"); every = e ? atof(e) : 2.0; }
    double now = plat_now_ns() / 1e9;
    if (every <= 0 || now - last < every) return;
    double dt = last ? now - last : every;
    last = now;
    if (!s_frames) return;
    extern const char *game_level_name(void);
    double n = (double)s_frames;
    char cuts[200] = "";
    int o = 0;
    for (int i = 0; i < 9; i++)
        if (s_cuts[i]) o += snprintf(cuts + o, sizeof cuts - (size_t)o, " %s %llu", cut_name[i], (unsigned long long)s_cuts[i]);
    LOG("[interp] %s: %.1f game frames/s, draws %.0f/frame matched %.1f%% (by object %.0f/f, by content %.0f/f - unchanged %.0f/f, by order %.0f/f -, new objects %.1f/f), buffers blended %.0f/f jumps %.0f/f (+%.0f/f with their class), toggling elements %.0f/image, whole-number %.0f, wraps %.1f, discontinuities %.1f | "
        "in-betweens %.1f/s, replay cpu %.2f ms avg %.2f max, prep %.2f ms/f | cuts:%s\n",
        game_level_name(), n / dt, s_draws / n, s_draws ? 100.0 * s_matched / s_draws : 0, s_id_pairs / n, s_content_pairs / n, s_same_pairs / n, s_occ_pairs / n, s_new_objects / n, s_blended_cbs / n, s_cut_cbs / n, s_class_snaps / n, s_replays ? (double)s_toggled / s_replays : 0, s_replays ? (double)s_integral / s_replays : 0, s_replays ? (double)s_wraps / s_replays : 0, s_replays ? (double)s_discont / s_replays : 0,
        s_replays / dt, s_replays ? s_replay_cpu / s_replays : 0, s_replay_cpu_max, s_prep / n, o ? cuts : " none");
    if (st_replay_misses || st_replay_cb_miss)
        LOG("[interp] replay draw misses %llu/%llu, uniform misses %llu\n", (unsigned long long)st_replay_misses,
            (unsigned long long)st_replay_draws, (unsigned long long)st_replay_cb_miss);
    s_frames = s_draws = s_matched = s_changed = s_cut_cbs = s_blended_cbs = s_id_pairs = s_content_pairs = s_same_pairs = s_occ_pairs = s_new_objects = s_class_snaps = s_toggled = s_integral = s_wraps = s_discont = 0;
    memset(s_cuts, 0, sizeof s_cuts);
    s_replays = 0;
    s_replay_cpu = s_replay_cpu_max = s_prep = 0;
    st_replay_misses = st_replay_draws = st_replay_cb_miss = 0;
}
