"""Map Odyssey 1.0.0 guest addresses to another version of main by symbol: port_sites.py OLD_MAIN NEW_MAIN ADDR...

Each address is located as <symbol>+<offset> in OLD_MAIN (the nearest preceding symbol), the same symbol is found in
NEW_MAIN, and inside it the instruction whose surroundings (a window of instructions, branch and page targets masked)
match best is reported with its score. For a data address (a vtable, a global) the symbol plus the same offset is
reported. Nothing is written: the output is read by a person and copied into tools/odyssey_versions.py.
"""
import bisect
import struct
import sys

from nso import Nso

BASE = 0x7000000000
WINDOW = 8   # instructions before and after the site that are compared


def masked(word):
    """An instruction word with what moves between builds removed: branch targets, adrp pages, literal offsets."""
    if (word & 0x7C000000) == 0x14000000:          # b, bl
        return word & 0xFC000000
    if (word & 0x9F000000) == 0x90000000:          # adrp
        return word & 0x9F00001F
    if (word & 0xFF000010) == 0x54000000:          # b.cond
        return word & 0xFF00001F
    if (word & 0x7E000000) == 0x34000000:          # cbz, cbnz
        return word & 0xFF00001F
    if (word & 0x7E000000) == 0x36000000:          # tbz, tbnz
        return word & 0xFFF8001F
    if (word & 0xFF800000) == 0x91000000:          # add (immediate, 64-bit): often a page offset after adrp
        return word & 0xFFC003FF
    return word


class Module:
    def __init__(self, path):
        self.n = Nso(path)
        syms = [(s[3], s[0], s[4]) for s in self.n.syms if s[0] and s[2] and s[3]]
        syms.sort()
        self.syms = syms
        self.addrs = [a for a, _, _ in syms]
        self.by_name = {}
        for a, name, size in syms:
            self.by_name.setdefault(name, (a, size))

    def word(self, off):
        return struct.unpack_from('<I', self.n.image, off)[0]

    def covering(self, off):
        i = bisect.bisect_right(self.addrs, off) - 1
        while i >= 0:
            a, name, size = self.syms[i]
            if size == 0 or off < a + size or i == 0:
                return a, name, size
            i -= 1
        return None

    def in_text(self, off):
        return self.n.text[0] <= off < self.n.text[0] + self.n.text[1]


def window(mod, off):
    return [masked(mod.word(off + 4 * k)) if 0 <= off + 4 * k < mod.n.size else None for k in range(-WINDOW, WINDOW + 1)]


def score(a, b):
    return sum(1 for x, y in zip(a, b) if x is not None and x == y)


def port(old, new, addr):
    off = addr - BASE
    cov = old.covering(off)
    if not cov:
        return f'{addr:#x}: no symbol before it'
    start, name, size = cov
    delta = off - start
    if name not in new.by_name:
        return f'{addr:#x}: {name}+{delta:#x} - symbol missing in the new main'
    nstart, nsize = new.by_name[name]
    if not old.in_text(off):
        return f'{addr:#x}: {name}+{delta:#x} (data) -> {BASE + nstart + delta:#x}'
    want = window(old, off)
    exact = old.word(off)
    span = max(nsize, size, delta + 4) + 0x100
    best = []
    for o in range(nstart, nstart + span, 4):
        s = score(want, window(new, o)) + (2 if new.word(o) == exact else 0)
        best.append((s, o))
    best.sort(reverse=True)
    (s1, o1), (s2, o2) = best[0], best[1]
    full = 2 * WINDOW + 1 + 2
    sure = 'exact' if s1 == full else ('ok' if s1 >= full - 3 and s1 - s2 >= 3 else 'CHECK')
    return (f'{addr:#x}: {name}+{delta:#x} -> {BASE + o1:#x} ({name}+{o1 - nstart:#x})  score {s1}/{full}, '
            f'next {s2} at {BASE + o2:#x}  word {exact:#010x}->{new.word(o1):#010x}  [{sure}]')


if __name__ == '__main__':
    old, new = Module(sys.argv[1]), Module(sys.argv[2])
    print(f'old {old.n.build_id[:16]} image {old.n.size:#x}, new {new.n.build_id[:16]} image {new.n.size:#x}')
    for a in sys.argv[3:]:
        print(port(old, new, int(a, 16)))
