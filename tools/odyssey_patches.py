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


# ModelKeeper::calc is called directly from the lifted game, bypassing dispatch overrides.
# Observe the models updated on the simulation tick so the extra draw can sample their skeletal clips.
PATCHES[0x700094c428] = _debug_hook
# StageScene::drawMain, just after copying the live LookAtCamera to its stack.
PATCHES[0x70004ccbc8] = _debug_hook


def _particle_tick(target):
    def patch(gen, a):
        # Both System::Draw and BatchCalculationComputeShaderEmitter can advance GPU particles.
        # Keep their draw and buffer handling intact while reusing particle positions on extra draws.
        ins = gen.lift(a)
        assert ins.kind == 'bl' and ins.target == target, f'{a:#x}: unexpected particle update call'
        code = ' '.join(gen.render_single(ins, a, None, set()))
        return (f'{{ extern _Bool odyssey_skip_extra_particle_update(void); extern void f_{target:x}(Cpu *); '
                f'if (!odyssey_skip_extra_particle_update()) {{ {code} }} }}')
    return patch


_particle_compute_tick = _particle_tick(0x7000b45ae8)
PATCHES[0x7000b398d0] = _particle_compute_tick
PATCHES[0x7000b39d2c] = _particle_compute_tick


for _h in filter(None, os.environ.get('ODYSSEY_HOOKS', '').replace(' ', '').split(',')):
    PATCHES[int(_h, 16)] = _debug_hook


# Super Mario Odyssey 1.4.1 (main build b424be150a8e7d78): each 1.0.0 site above at its place in 1.4.1's main, found by
# tools/port141.py (the same instruction word in the matched function) and read by eye with tools/dis141.py.
ODYSSEY_141_BUILD = 'b424be150a8e7d78'
SITES_141 = {
    HEAP_LOCK_SITE: 0x7000954ea4,   # sead::Heap::Heap + 0xe4: csel w8, w8, w9, ne (same word)
    0x7000837914: 0x70005adf40,     # al::CameraPoseUpdater::init: ldp s3, s1 (same word)
    0x7000837a54: 0x70005ae094,     # al::CameraPoseUpdater::update: ldp s1, s3 (same word)
    0x7000a63c94: 0x70006330d4,     # al::initDrawInfoDefault: ucvtf after the layout width (now width(0))
    0x7000a63cb4: 0x70006330fc,     # ... the second width
    0x70001e813c: 0x70001ca548,     # CounterLifeCtrl::calcLayoutTransByPlayer: ucvtf s0 after the width
    0x70001e816c: 0x70001ca57c,     # ... ucvtf s1 (same word)
    0x70003fb8b0: 0x70003a1008,     # HackCap::updateFrameOutLayout: ucvtf s0 after the width
    0x7000883064: 0x70005fcdc4,     # alEffectSystemFunction::drawEffect2D: ucvtf s8 after the width
    0x700094c428: 0x70006b9ec0,     # al::ModelKeeper::calc's entry (debug hook)
    0x70004ccbc8: 0x7000473354,     # StageScene::drawMain, after the camera copy (debug hook)
    0x7000b398d0: 0x700091e0ac,     # nn::vfx::System::DrawEmitter: bl CalculateComputeShader
    0x7000b39d2c: 0x700091e2a4,     # nn::vfx::System::BatchCalculationComputeShaderEmitter: bl CalculateComputeShader
}
CALC_COMPUTE_SHADER_141 = 0x700092c020  # nn::vfx::detail::EmitterCalculator::CalculateComputeShader (1.0.0: 0x7000b45ae8)

# The 1.4.1 table: the same replacements at the 1.4.1 sites. The wide-screen sites keep their instruction words; in
# 1.4.1's al::initDrawInfoDefault the DrawInfo is in x19 (1.0.0: x20).
WIDE_SITES_141 = {SITES_141[a]: v for a, v in WIDE_SITES.items()}
WIDE_SITES_141[SITES_141[0x7000a63c94]] = (0x1e230000, 0, 'odyssey_wide_drawinfo(c->x[19])')


def _wide_141(gen, a):
    word, reg, k = WIDE_SITES_141[a]
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == word, f'{a:#x}: {w:#x}'
    code = ' '.join(gen.render_single(gen.lift(a), a, None, set()))
    decl = 'extern float odyssey_wide(void), odyssey_wide_camera(void), odyssey_wide_drawinfo(uint64_t);'
    return f'{code} {{ {decl} FP_SETS({reg}, VSF32({reg}) * {k}); }}  /* {a:#x}: wide screen (1.4.1) */'


PATCHES_141 = {SITES_141[HEAP_LOCK_SITE]: _heap_lock}
for _a in WIDE_SITES_141:
    PATCHES_141[_a] = _wide_141
PATCHES_141[SITES_141[0x700094c428]] = _debug_hook
PATCHES_141[SITES_141[0x70004ccbc8]] = _debug_hook
PATCHES_141[SITES_141[0x7000b398d0]] = _particle_tick(CALC_COMPUTE_SHADER_141)
PATCHES_141[SITES_141[0x7000b39d2c]] = _particle_tick(CALC_COMPUTE_SHADER_141)
for _h in filter(None, os.environ.get('ODYSSEY_HOOKS', '').replace(' ', '').split(',')):
    PATCHES_141[int(_h, 16)] = _debug_hook

# Functions replaced by the runtime (runtime/hle/hle_symbols.txt) that 1.4.1's main has no names for: address -> name
HLE_141 = {
    0x700097bfa0: '_ZN4sead6system4HaltEv',
    0x70004dba00: '_ZN14GameDataHolder44readFromSaveDataBufferCommonFileOnlyLanguageEv',  # (code shape: odyssey_language.c)
    # the mouse camera without a stick's limit (runtime/hle/input.c); 1.4.1 had none, so the mouse was the right stick
    # there, capped at full tilt (#11). ProjectCameraInput's vtable slot 0, a branch to calcCameraMoveInput (0x70005172b0)
    0x70000c1310: '_ZNK18ProjectCameraInput14calcInputStickEPN4sead7Vector2IfEE',
}

# main's build -> its patches (gen.py; other modules and builds are lifted unpatched)
PATCHES_BY_BUILD = {ODYSSEY_MAIN_BUILD: PATCHES, ODYSSEY_141_BUILD: PATCHES_141}
HLE_BY_BUILD = {ODYSSEY_141_BUILD: HLE_141}
