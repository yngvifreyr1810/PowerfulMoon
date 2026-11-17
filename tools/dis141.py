"""Two builds of main side by side: dis141.py OLD_MAIN NEW_MAIN OLD_ADDR NEW_ADDR [COUNT]
(hex addresses; COUNT instructions from each, default 40) - for checking a matched function or patch site by eye."""
import sys

import capstone

from nso import Nso

BASE = 0x7000000000


def lines(path, addr, n):
    img = Nso(path).image
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    off = addr - BASE
    out = [f'{i.address:x}: {i.mnemonic:7} {i.op_str}' for i in md.disasm(bytes(img[off:off + 4 * n]), addr)]
    return out + [''] * (n - len(out))


if __name__ == '__main__':
    n = int(sys.argv[5]) if len(sys.argv) > 5 else 40
    a = lines(sys.argv[1], int(sys.argv[3], 16), n)
    b = lines(sys.argv[2], int(sys.argv[4], 16), n)
    for x, y in zip(a, b):
        print(f'{x:<58}| {y}')
