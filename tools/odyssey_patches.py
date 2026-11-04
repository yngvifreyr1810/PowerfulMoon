"""Per-instruction replacements for Super Mario Odyssey 1.0.0 (main, build ODYSSEY_MAIN_BUILD), applied by gen.py
(guest address -> function(gen, addr) returning C code).

sead heaps with their lock always on: sead::Heap::Heap only uses its CriticalSection when created with enableLock,
and Odyssey creates e.g. "StationedResourceHeap" without one: its loader threads (al::AsyncFunctorThread, e.g.
HakoniwaSequence::initSystem, WorldResourceLoader) run on the main thread's core at a lower priority, so on the
Switch they never run at the same time as it. Here every guest thread runs in parallel: an allocation now and then
failed in a consistent heap with 392 MB free (sead halt at boot). The constructor's
`csel w8, w8, w9, ne` (flags | 1 if enableLock, else flags & ~1) keeps flags | 1.
"""
import os
import struct

ODYSSEY_MAIN_BUILD = '3ca12dfaaf9c82da'

HEAP_LOCK_SITE = 0x7000743fa0  # sead::Heap::Heap(name, parent, start, size, direction, enableLock) + 0xe8


def _heap_lock(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x1a891108, hex(w)  # csel w8, w8, w9, ne
    return f'c->x[8] = (uint32_t)c->x[8];  /* {a:#x}: csel w8, w8, w9, ne -> heap lock always on */'


PATCHES = {HEAP_LOCK_SITE: _heap_lock}


# Wide screen (runtime/hle/odyssey_wide.c): a value the game takes for its 16:9 screen, times odyssey_wide() (the
# window's aspect / 16:9 when the image fills a wider window, else 1) right after the instruction that produces it.
# The camera: al::CameraPoseUpdater hands its aspect (+0xc0, 16:9) to al::Projection::setProj (in s3): a wider view at
# the same height (Hor+), and culling, shadows and al::calcLayoutPosFromWorldPos (layout width = aspect x 720) follow.
# The layouts: the width al::getLayoutDisplayWidth() returns (1280) where it sizes the screen - the layouts' projection
# (al::initDrawInfoDefault: the HUD keeps its shape, centred, with room at the sides), the life gauge and Cappy's
# off-screen marker kept on the screen, the 2D effects' projection. Not where StageScene / TitleMenuScene::drawMain
# make the layouts' viewport from it: that stays the whole target.
WIDE_SITES = {  # address: (instruction word, s register to widen, the factor)
    # al::CameraPoseUpdater::init: ldp s3, s1, [x19, #0xc0]
    0x7000837914: (0x2d580663, 3, 'odyssey_wide()'),
    # al::CameraPoseUpdater::update: ldp s1, s3, [x19, #0xbc] (where a new factor takes effect)
    0x7000837a54: (0x2d578e61, 3, 'odyssey_wide_camera()'),
    # al::initDrawInfoDefault: ucvtf s0, w0 (left; x20 = the DrawInfo), ucvtf s0, w0 (right)
    0x7000a63c94: (0x1e230000, 0, 'odyssey_wide_drawinfo(c->x[20])'),
    0x7000a63cb4: (0x1e230000, 0, 'odyssey_wide()'),
    # CounterLifeCtrl::calcLayoutTransByPlayer: ucvtf s0, w0 (left limit), ucvtf s1, w0 (right limit)
    0x70001e813c: (0x1e230000, 0, 'odyssey_wide()'),
    0x70001e816c: (0x1e230001, 1, 'odyssey_wide()'),
    # HackCap::updateFrameOutLayout: ucvtf s0, w0
    0x70003fb8b0: (0x1e230000, 0, 'odyssey_wide()'),
    # alEffectSystemFunction::drawEffect2D: ucvtf s8, w0
    0x7000883064: (0x1e230008, 8, 'odyssey_wide()'),
}


def _wide(gen, a):
    word, reg, k = WIDE_SITES[a]
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == word, f'{a:#x}: {w:#x}'
    code = ' '.join(gen.render_single(gen.lift(a), a, None, set()))
    decl = 'extern float odyssey_wide(void), odyssey_wide_camera(void), odyssey_wide_drawinfo(uint64_t);'
    return f'{code} {{ {decl} FP_SETS({reg}, VSF32({reg}) * {k}); }}  /* {a:#x}: wide screen */'


for _a in WIDE_SITES:
    PATCHES[_a] = _wide


# Debug hooks (make gen with ODYSSEY_HOOKS=<addr>[,<addr>...], hex): each listed instruction first calls the runtime's
# odyssey_debug_hook(c, addr) (runtime/hle/odyssey_debug.c, ODYSSEY_HOOK_LOG says what it logs), then runs as usual.
# Only instructions that fall through or return (no branches).
def _debug_hook(gen, a):
    ins = gen.lift(a)
    assert ins.kind in ('op', 'asm', 'trap', 'ret'), f'{a:#x}: no hook on a {ins.kind}'
    code = ' '.join(gen.render_single(ins, a, None, set()))
    return f'{{ extern void odyssey_debug_hook(Cpu *, uint64_t); odyssey_debug_hook(c, {a:#x}ull); }} {code}'


for _h in filter(None, os.environ.get('ODYSSEY_HOOKS', '').replace(' ', '').split(',')):
    PATCHES[int(_h, 16)] = _debug_hook
