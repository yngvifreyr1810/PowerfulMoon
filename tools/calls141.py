"""Two functions' calls side by side: calls141.py OLD_MAIN NEW_MAIN OLD_FN NEW_FN (hex)
Each bl in order, with the callee's name (1.0.0) and what tools/match141.py maps it to - for following a call chain in
1.4.1 next to the same chain in 1.0.0."""
import pickle
import os
import sys

from match141 import Build, Matcher

BASE = 0x7000000000
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'match141.pickle')


def gmap(old, new):
    if os.path.exists(CACHE):
        return pickle.load(open(CACHE, 'rb'))
    g = Matcher(old, new).global_map()
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    pickle.dump(g, open(CACHE, 'wb'))
    return g


if __name__ == '__main__':
    old, new = Build(sys.argv[1]), Build(sys.argv[2])
    of, nf = int(sys.argv[3], 16) - BASE, int(sys.argv[4], 16) - BASE
    g = gmap(old, new)
    inv = {v[0]: k for k, v in g.items()}
    names = {}
    for name, (v, _) in old.sym.items():
        names.setdefault(v, name)
    oc, nc = old.calls(*old.extent(of)), new.calls(*new.extent(nf))
    print(f'old {BASE + of:#x}: {len(oc)} calls, new {BASE + nf:#x}: {len(nc)} calls')
    for k in range(max(len(oc), len(nc))):
        a = oc[k] if k < len(oc) else None
        b = nc[k] if k < len(nc) else None
        an = (names.get(a, '') or '')[:70] if a is not None else ''
        mark = '=' if a is not None and a in g and g[a][0] == b else ' '
        bo = inv.get(b)
        bn = (names.get(bo, '') or '')[:50] if bo is not None else ''
        print(f'{k:3} {BASE + a:#x} {an:<70} {mark} {BASE + b:#x} {bn}' if a is not None and b is not None
              else f'{k:3} {"" if a is None else hex(BASE + a)} {an:<70}   {"" if b is None else hex(BASE + b)} {bn}')
