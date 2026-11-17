"""Which game symbols the runtime looks up by name that 1.4.1's main no longer exports: v141_names.py OLD_MAIN NEW_MAIN

Collects every mangled name in runtime/ (string literals "_Z..."), keeps those defined in 1.0.0's main (the SDK's own
names stay in sdk / subsdk0, which 1.4.1 still exports), and reports which of them 1.4.1's main lacks - each one needs
an address found by matching (tools/match141.py).
"""
import glob
import os
import re
import sys

from nso import Nso

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')


def runtime_names():
    names = {}
    for path in glob.glob(os.path.join(ROOT, 'runtime', '**', '*.*'), recursive=True):
        if not path.endswith(('.c', '.h', '.mm', '.cpp', '.txt')):
            continue
        text = open(path, encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'"(_Z[A-Za-z0-9_]+)"', text):
            names.setdefault(m.group(1), set()).add(os.path.relpath(path, ROOT))
        if path.endswith('hle_symbols.txt'):
            for line in text.splitlines():
                s = line.split('#', 1)[0].strip()
                if s.startswith('_Z'):
                    names.setdefault(s.split()[0], set()).add(os.path.relpath(path, ROOT))
    return names


def exported(nso):
    return {s[0]: s[3] for s in nso.syms if s[0] and s[2]}


if __name__ == '__main__':
    old, new = Nso(sys.argv[1]), Nso(sys.argv[2])
    old_ex, new_ex = exported(old), exported(new)
    names = runtime_names()
    in_main = {n: f for n, f in names.items() if n in old_ex}
    missing = sorted(n for n in in_main if n not in new_ex)
    print(f'{len(names)} names in runtime/, {len(in_main)} defined in 1.0.0 main, {len(missing)} not exported by 1.4.1 main')
    for n in missing:
        print(f'{old_ex[n] + 0x7000000000:#x}  {n}  [{", ".join(sorted(in_main[n]))}]')
