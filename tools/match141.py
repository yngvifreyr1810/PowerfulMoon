"""Find 1.0.0 functions in another (stripped) build of main: match141.py OLD_MAIN NEW_MAIN [NAME...]

1.4.1's main exports almost no symbols, so the runtime's hooks (looked up by name in 1.0.0) need addresses found by
matching. Three ways, best first:

1. vtables (C++ RTTI survives stripping): a class's typeinfo name ("10StageScene") leads to its typeinfo object, that
   to its vtable; a virtual function sits in the same slot as in 1.0.0's vtable of the class.
2. code shape: each function's instructions with what moves between builds masked (branch targets, pages, literal
   offsets); a body that occurs exactly once in both builds is the same function.
3. call graph: an unmatched function called from a matched one, at the same call position, is taken from there.

Prints NAME -> address, the method and the old/new sizes. With no NAME: the runtime's list (tools/v141_names.py).
"""
import bisect
import hashlib
import struct
import sys
from collections import defaultdict

from analyze import eh_frame_functions
from nso import Nso, R_AARCH64_RELATIVE
from port_sites import masked

BASE = 0x7000000000


class Build:
    def __init__(self, path):
        self.n = Nso(path)
        self.text = self.n.text
        img = self.n.image
        starts = set(eh_frame_functions(self.n))
        self.sym = {}
        for name, info, shndx, value, size in self.n.syms:
            if name and shndx:
                self.sym[name] = (value, size)
                if (info & 0xF) == 2 and value:
                    starts.add(value)
        self.relocs = {r[0]: r[3] for r in self.n.relocs if r[1] == R_AARCH64_RELATIVE}
        for a in self.relocs.values():
            if self.text[0] <= a < self.text[0] + self.text[1]:
                starts.add(a)
        self.starts = sorted(a for a in starts if self.text[0] <= a < self.text[0] + self.text[1])
        self.img = img

    def word(self, off):
        return struct.unpack_from('<I', self.img, off)[0]

    def extent(self, start, size=0):
        """[start, end) of the function at start: its symbol size, else up to the next start."""
        if size:
            return start, start + size
        i = bisect.bisect_right(self.starts, start)
        end = self.starts[i] if i < len(self.starts) else self.text[0] + self.text[1]
        while end > start + 4 and self.word(end - 4) == 0:  # (padding)
            end -= 4
        return start, end

    def shape(self, start, end):
        h = hashlib.sha1()
        for o in range(start, end, 4):
            h.update(struct.pack('<I', masked(self.word(o))))
        return h.hexdigest()

    def calls(self, start, end):
        """bl targets in order."""
        out = []
        for o in range(start, end, 4):
            w = self.word(o)
            if (w & 0xFC000000) == 0x94000000:
                imm = w & 0x3FFFFFF
                if imm & (1 << 25):
                    imm -= 1 << 26
                out.append(o + imm * 4)
        return out

    def find_bytes(self, needle):
        hits, i = [], self.img.find(needle)
        while i >= 0:
            hits.append(i)
            i = self.img.find(needle, i + 1)
        return hits

    def vtable_of(self, typeinfo_name):
        """The vtable (offset of its first function slot) of the class whose RTTI name is typeinfo_name, or None."""
        names = [o for o in self.find_bytes(typeinfo_name.encode() + b'\0')
                 if o == 0 or self.img[o - 1] == 0]
        for name_off in names:
            # typeinfo: [vptr of the typeinfo class][pointer to the name]...: a RELATIVE reloc whose addend is the name
            for ti_name_slot in [r for r, a in self.relocs.items() if a == name_off]:
                ti = ti_name_slot - 8
                # vtable: [offset to top = 0][typeinfo pointer][slot 0]...: a RELATIVE reloc whose addend is ti
                for vt_ti_slot in [r for r, a in self.relocs.items() if a == ti]:
                    if struct.unpack_from('<q', self.img, vt_ti_slot - 8)[0] == 0:
                        return vt_ti_slot + 8
        return None


def demangled_class(name):
    """'_ZNK10StageScene8drawMainEv' -> '10StageScene' (the typeinfo name of the class a method belongs to)."""
    s = name[3:] if name.startswith('_ZN') else None
    if not s:
        return None
    if s.startswith('K'):
        s = s[1:]
    parts, i = [], 0
    while i < len(s) and s[i].isdigit():
        j = i
        while s[j].isdigit():
            j += 1
        n = int(s[i:j])
        parts.append(s[i:j + n])
        i = j + n
    if len(parts) < 2:
        return None
    cls = parts[:-1]
    return cls[0] if len(cls) == 1 else 'N' + ''.join(cls) + 'E'


class Matcher:
    def __init__(self, old, new):
        self.old, self.new = old, new
        self.new_by_shape = defaultdict(list)
        for s in new.starts:
            self.new_by_shape[new.shape(*new.extent(s))].append(s)
        self.old_shape_count = defaultdict(int)
        for s in old.starts:
            self.old_shape_count[old.shape(*old.extent(s))] += 1

    def by_vtable(self, name):
        old, new = self.old, self.new
        cls = demangled_class(name)
        if not cls or name not in old.sym:
            return None
        addr = old.sym[name][0]
        ovt, nvt = old.vtable_of(cls), new.vtable_of(cls)
        if ovt is None or nvt is None:
            return None
        for slot in range(0, 400):
            o = ovt + slot * 8
            if old.relocs.get(o) == addr:
                return new.relocs.get(nvt + slot * 8), f'vtable {cls} slot {slot}'
        return None

    def by_shape(self, name):
        old = self.old
        if name not in old.sym:
            return None
        start, size = old.sym[name]
        sh = old.shape(*old.extent(start, size))
        hits = self.new_by_shape.get(sh, [])
        if len(hits) == 1 and self.old_shape_count[sh] == 1:
            return hits[0], 'code shape'
        return None

    def global_map(self, rounds=6):
        """old address -> (new address, how): every class's vtable slots, unique shapes, then the call graph."""
        old, new = self.old, self.new
        found, bad = {}, set()

        def add(o, n, how):
            if o in bad or n is None:
                return
            if o in found and found[o][0] != n:
                bad.add(o)
                del found[o]
                return
            found.setdefault(o, (n, how))

        # 1. every class with a vtable in 1.0.0 (symbols _ZTV<typeinfo name>)
        new_vt_cache = {}
        for name, (value, size) in old.sym.items():
            if not name.startswith('_ZTV') or not size:
                continue
            cls = name[4:]
            nvt = new_vt_cache.setdefault(cls, new.vtable_of(cls))
            if nvt is None:
                continue
            ovt = value + 16
            for slot in range((size - 16) // 8):
                oa, na = old.relocs.get(ovt + slot * 8), new.relocs.get(nvt + slot * 8)
                if oa and na and old.text[0] <= oa < old.text[0] + old.text[1] and new.text[0] <= na < new.text[0] + new.text[1]:
                    add(oa, na, f'vtable {cls}')
        print(f'  vtables: {len(found)} functions')
        # 2. unique code shapes
        for s in old.starts:
            sh = old.shape(*old.extent(s))
            hits = self.new_by_shape.get(sh, [])
            if len(hits) == 1 and self.old_shape_count[sh] == 1:
                add(s, hits[0], 'code shape')
        print(f'  + shapes: {len(found)} functions')
        # 3. the call graph: same number of calls -> the same callees, in order
        for r in range(rounds):
            before = len(found)
            for o, (n, _) in list(found.items()):
                oc, nc = old.calls(*old.extent(o)), new.calls(*new.extent(n))
                if len(oc) != len(nc) or not oc:
                    continue
                for a, b in zip(oc, nc):
                    if a not in found:
                        add(a, b, 'call graph')
            print(f'  + calls round {r + 1}: {len(found)} functions')
            if len(found) == before:
                break
        return found

    def match(self, name):
        for f in (self.by_vtable, self.by_shape):
            r = f(name)
            if r and r[0] is not None:
                return r
        if not hasattr(self, 'gmap'):
            self.gmap = self.global_map()
        o = self.old.sym.get(name, (None,))[0]
        return self.gmap.get(o)

    # ------------------------------------------------------------ fuzzy: near matched neighbours
    def grams(self, b, start, end, k=3):
        w = [masked(b.word(o)) for o in range(start, end, 4)]
        return {tuple(w[i:i + k]) for i in range(max(len(w) - k + 1, 1))}

    def reverse_calls(self, b):
        rc = defaultdict(set)
        for s in b.starts:
            for t in b.calls(*b.extent(s)):
                rc[t].add(s)
        return rc

    def fuzzy(self, name, top=4):
        """Candidates for an unmatched function: the unmatched callees of its matched callers' counterparts and the
        unmatched callers of its matched callees' counterparts, scored by instruction 3-gram overlap and size."""
        old, new = self.old, self.new
        if not hasattr(self, 'gmap'):
            self.gmap = self.global_map()
        if not hasattr(self, 'orc'):
            self.orc, self.nrc = self.reverse_calls(old), self.reverse_calls(new)
            self.new_taken = {n for n, _ in self.gmap.values()}
        a, size = old.sym[name]
        os_, oe = old.extent(a, size)
        cands = set()
        for caller in self.orc.get(a, ()):
            if caller in self.gmap:
                cands.update(t for t in new.calls(*new.extent(self.gmap[caller][0])) if t not in self.new_taken)
        for callee in old.calls(os_, oe):
            if callee in self.gmap:
                cands.update(c for c in self.nrc.get(self.gmap[callee][0], ()) if c not in self.new_taken)
        og = self.grams(old, os_, oe)
        scored = []
        for c in cands:
            cs, ce = new.extent(c)
            if not (0.4 < (ce - cs) / max(oe - os_, 4) < 2.5):
                continue
            ng = self.grams(new, cs, ce)
            j = len(og & ng) / max(len(og | ng), 1)
            scored.append((j, c, ce - cs))
        scored.sort(reverse=True)
        callers = sum(1 for c in self.orc.get(a, ()) if c in self.gmap)
        return scored[:top], len(cands), callers, len(self.orc.get(a, ()))


DEFAULT = [
    '_ZN10GameSystem8movementEv', '_ZN19PlayerActorHakoniwa8movementEv', '_ZN2al15GameFrameworkNx9procDraw_Ev',
    '_ZNK10StageScene8drawMainEv', '_ZNK20DemoChangeWorldScene8drawMainEv', '_ZN4sead15GameFrameworkNx9procCalc_Ev',
    '_ZN4sead13ControllerMgr4calcEv', '_ZN3agl3utl23DynamicTextureAllocator4calcEv', '_ZN2al18updateGraphicsPrevEPNS_5SceneE',
    '_ZNK4sead12LookAtCamera14doUpdateMatrixEPNS_8Matrix34IfEE', '_ZN2al13AnimPlayerSkl11calcSklAnimEv',
    '_ZN2nn3g3d11SkeletonObj17CalculateWorldMtxERKNS_4util4neon22MatrixRowMajor4x3fTypeE',
    '_ZN2nn3g3d11SkeletonObj17CalculateSkeletonEi', '_ZN4sead6system14HaltWithDetailEPKciS2_z',
    '_ZN4sead6system15PrintStringImplEPKci', '_ZN4sead6system4HaltEv',
    '_ZN10StageScene4initERKN2al13SceneInitInfoE', '_ZN10StageScene7controlEv',
    # functions holding the address patches (tools/odyssey_patches.py)
    '_ZN4sead4HeapC2ERKNS_14SafeStringBaseIcEEPS0_PvmNS0_13HeapDirectionEb',
    '_ZN2al17CameraPoseUpdater4initEPKNS_19CameraParamTransferEPKNS_15CameraStopJudgeEPNS_20CameraStartParamCtrlE',
    '_ZN2al17CameraPoseUpdater6updateEv', '_ZN2al19initDrawInfoDefaultEPN2nn4ui2d8DrawInfoEPNS1_16GraphicsResourceE',
    '_ZN15CounterLifeCtrl23calcLayoutTransByPlayerEPN4sead7Vector3IfEE', '_ZN7HackCap20updateFrameOutLayoutEv',
    '_ZN22alEffectSystemFunction12drawEffect2DEPKN2al12EffectSystemEPKN4sead8ViewportE',
    '_ZN2al11ModelKeeper4calcERKN4sead8Matrix34IfEERKNS1_7Vector3IfEE', '_ZN2al18GraphicsSystemInfo13updateViewGpuEiPKN4sead6CameraEPKNS_10ProjectionE',
]


if __name__ == '__main__':
    old, new = Build(sys.argv[1]), Build(sys.argv[2])
    names = sys.argv[3:] or DEFAULT
    m = Matcher(old, new)
    print(f'old: {len(old.starts)} functions, new: {len(new.starts)} functions; '
          f'{sum(1 for v in m.new_by_shape.values() if len(v) == 1)} unique shapes in new')
    found = 0
    for n in names:
        r = m.match(n)
        o = old.sym.get(n, (0, 0))
        if r:
            found += 1
            ns, ne = new.extent(r[0])
            print(f'{BASE + o[0]:#x} -> {BASE + r[0]:#x}  size {o[1]:#x}/{ne - ns:#x}  {r[1]:<32} {n}')
        else:
            print(f'{BASE + o[0]:#x} -> ?            size {o[1]:#x}{"":>40} {n}')
            if n in old.sym:
                best, ncand, mcallers, ncallers = m.fuzzy(n)
                print(f'      {ncand} candidates near {mcallers} of {ncallers} callers (matched): ' +
                      ', '.join(f'{BASE + c:#x} ({s:#x}) {j:.2f}' for j, c, s in best))
    print(f'{found} of {len(names)} matched')
