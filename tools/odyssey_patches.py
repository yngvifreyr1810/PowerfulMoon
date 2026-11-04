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
