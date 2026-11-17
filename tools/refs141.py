"""Where code uses a global (adrp + ldr/add/str), in both builds: refs141.py OLD_MAIN NEW_MAIN OLD_GLOBAL [OLD_FUNC...]

For a data symbol of 1.0.0 (e.g. agl::utl::DynamicTextureAllocator::sInstance), lists the 1.0.0 functions that
reference it, their 1.4.1 counterparts (tools/match141.py's map), and the globals those reference at the same place:
the 1.4.1 address of the global. Also prints the bl at the given 1.0.0 call sites (hex) mapped the same way.
"""
import sys
from collections import Counter

import capstone

from match141 import Build, Matcher

BASE = 0x7000000000
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
md.detail = True


def global_refs(b, start, end):
    """[(offset of the use, absolute target)] for adrp xN + (ldr|add|str) [xN, #imm] pairs in [start, end)."""
    out, pages = [], {}
    for i in md.disasm(bytes(b.img[start:end]), start):
        ops = i.operands
        if i.mnemonic == 'adrp':
            pages[ops[0].reg] = ops[1].imm
        elif i.mnemonic in ('ldr', 'str', 'add', 'ldrb', 'ldrh', 'ldrsw', 'strb'):
            if i.mnemonic == 'add' and len(ops) == 3 and ops[1].reg in pages and ops[2].type == capstone.arm64.ARM64_OP_IMM:
                out.append((i.address, pages[ops[1].reg] + ops[2].imm))
            elif len(ops) == 2 and ops[1].type == capstone.arm64.ARM64_OP_MEM and ops[1].mem.base in pages:
                out.append((i.address, pages[ops[1].mem.base] + ops[1].mem.disp))
    return out


if __name__ == '__main__':
    old, new = Build(sys.argv[1]), Build(sys.argv[2])
    target = int(sys.argv[3], 16) - BASE
    m = Matcher(old, new)
    gmap = m.global_map()
    votes = Counter()
    for s in old.starts:
        refs = global_refs(old, *old.extent(s))
        hits = [k for k, (o, t) in enumerate(refs) if t == target]
        if not hits or s not in gmap:
            continue
        n = gmap[s][0]
        nrefs = global_refs(new, *new.extent(n))
        if len(nrefs) == len(refs):
            for k in hits:
                votes[nrefs[k][1]] += 1
                print(f'  {BASE + s:#x} -> {BASE + n:#x}: use {k} -> {BASE + nrefs[k][1]:#x}')
    for t, c in votes.most_common(3):
        print(f'{BASE + target:#x} -> {BASE + t:#x} ({c} functions agree)')
