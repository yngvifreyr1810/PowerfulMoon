"""The 1.0.0 patch sites (tools/odyssey_patches.py, runtime hooks by address) inside their 1.4.1 functions:
port141.py OLD_MAIN NEW_MAIN

FUNCS maps each patched 1.0.0 function to its 1.4.1 start (from tools/match141.py: vtables, code shape, call graph,
or its fuzzy candidates checked by hand). For every site the instruction window around it is looked for in the new
function (port_sites.py's masking); a site whose own instruction word is found again, in the best window, confirms the
function match too. Nothing is written: the result goes into tools/odyssey_versions.py by hand.
"""
import struct
import sys

from match141 import Build
from port_sites import masked, WINDOW

BASE = 0x7000000000

# 1.0.0 function start -> 1.4.1 function start (main, offsets from BASE)
FUNCS = {
    0x743eb8: 0x954dc0,   # sead::Heap::Heap (fuzzy 0.42)
    0x837898: 0x5adec0,   # al::CameraPoseUpdater::init (fuzzy, next to update)
    0x83794c: 0x5adf80,   # al::CameraPoseUpdater::update (fuzzy 0.49)
    0xa63c70: 0x6330b0,   # al::initDrawInfoDefault (call graph)
    0x1e7f88: 0x1ca390,   # CounterLifeCtrl::calcLayoutTransByPlayer (fuzzy 0.55)
    0x3fb788: 0x3a0ee0,   # HackCap::updateFrameOutLayout (fuzzy 0.49)
    0x883034: 0x5fcd90,   # alEffectSystemFunction::drawEffect2D (fuzzy 0.23)
    0x94c428: 0x6b9ec0,   # al::ModelKeeper::calc (fuzzy 0.45)
    0x4ccaf8: 0x473270,   # StageScene::drawMain (vtable)
    0x879c98: 0x5e77f0,   # al::GraphicsSystemInfo::updateViewGpu (fuzzy 0.33)
}

# the sites: address -> what it is (tools/odyssey_patches.py, runtime/hle/odyssey_debug.c)
SITES = {
    0x7000743fa0: 'heap lock: csel w8, w8, w9, ne',
    0x7000837914: 'wide: CameraPoseUpdater::init ldp s3, s1',
    0x7000837a54: 'wide: CameraPoseUpdater::update ldp s1, s3',
    0x7000a63c94: 'wide: initDrawInfoDefault ucvtf (left)',
    0x7000a63cb4: 'wide: initDrawInfoDefault ucvtf (right)',
    0x70001e813c: 'wide: CounterLifeCtrl ucvtf s0',
    0x70001e816c: 'wide: CounterLifeCtrl ucvtf s1',
    0x70003fb8b0: 'wide: HackCap ucvtf s0',
    0x7000883064: 'wide: drawEffect2D ucvtf s8',
    0x700094c428: 'debug hook: ModelKeeper::calc entry',
    0x70004ccbc8: 'debug hook: StageScene::drawMain camera copy',
    0x7000879c98: 'debug hook: updateViewGpu entry (DLSS)',
}


def window(b, off):
    return [masked(b.word(off + 4 * k)) for k in range(-WINDOW, WINDOW + 1)]


def main():
    old, new = Build(sys.argv[1]), Build(sys.argv[2])
    starts = sorted(FUNCS)
    for addr, what in SITES.items():
        off = addr - BASE
        fo = max((s for s in starts if s <= off), default=None)
        if fo is None or off - fo > 0x4000:
            print(f'{addr:#x}: no mapped function ({what})')
            continue
        fn = FUNCS[fo]
        _, end = new.extent(fn)
        want, exact = window(old, off), old.word(off)
        best = []
        for o in range(fn, end, 4):
            got = window(new, o)
            s = sum(1 for x, y in zip(want, got) if x == y) + (3 if new.word(o) == exact else 0)
            best.append((s, o))
        best.sort(reverse=True)
        (s1, o1), (s2, _) = best[0], best[1] if len(best) > 1 else (0, 0)
        full = 2 * WINDOW + 1 + 3
        same = new.word(o1) == exact
        verdict = 'OK' if same and s1 >= full // 2 and s1 - s2 >= 3 else 'CHECK'
        print(f'{addr:#x} -> {BASE + o1:#x}  +{off - fo:#x} -> +{o1 - fn:#x}  score {s1}/{full} (next {s2})  '
              f'word {exact:#010x}{"=" if same else "!="}{new.word(o1):#010x}  [{verdict}]  {what}')


if __name__ == '__main__':
    main()
