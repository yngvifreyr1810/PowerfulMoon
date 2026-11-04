#!/usr/bin/env python3
"""Shipped list of the programs a run through the game drew (runtime/nvn/vulkan/known_programs.txt).

The renderer records every program it draws in cache/drawn_programs.bin: a 64-bit hash of the start of the program's
two shaders (bit 0: drawn while builds block - in a scene's first frames, on the title, while loading). This turns that
file into the text list shipped with the program, merged with the list as it is, so a first start compiles those
programs ahead too (runtime/nvn/vulkan/exec.c renderer_program_hint). Hashes only: nothing of the game is in it.

    python tools/known_programs.py [cache/drawn_programs.bin ...]
"""
import os
import struct
import sys

OUT = os.path.join(os.path.dirname(__file__), '..', 'runtime', 'nvn', 'vulkan', 'known_programs.txt')


def main():
    progs = {}  # hash (bit 0 cleared) -> warm
    if os.path.exists(OUT):
        for line in open(OUT):
            parts = line.split()
            if len(parts) == 2 and not line.startswith('#'):
                h = int(parts[0], 16) & ~1
                progs[h] = progs.get(h, False) or parts[1] == '1'
    before = len(progs)
    for path in sys.argv[1:] or ['cache/drawn_programs.bin']:
        data = open(path, 'rb').read()
        for (v,) in struct.iter_unpack('<Q', data[:len(data) // 8 * 8]):
            if v:
                progs[v & ~1] = progs.get(v & ~1, False) or bool(v & 1)
    with open(OUT, 'w', newline='\n') as f:
        f.write('# Programs drawn in a run through the game: hash, drawn in a scene\'s first frames (tools/known_programs.py)\n')
        for h in sorted(progs):
            f.write('%016x %d\n' % (h, progs[h]))
    warm = sum(progs.values())
    print(f'{len(progs)} programs ({len(progs) - before} new), {warm} warm -> {os.path.normpath(OUT)}')


if __name__ == '__main__':
    main()
