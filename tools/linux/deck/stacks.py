#!/usr/bin/env python3
# Summarise build/stacks.txt (runtime/sampler.c, ODYSSEY_SAMPLE_STACK=1) on the Deck: our addresses named with
# addr2line (and the game library's, odyssey_game.so next to it, by its symbols), then inclusive counts per function
# and the callers of a function given as the second argument.
#   python3 stacks.py <binary> [function]      (in the folder with stacks.txt)
import collections, os, subprocess, sys
B = sys.argv[1]
G = None
for d in (os.path.dirname(B), os.path.join(os.path.dirname(B), "..", "lib"), "/run/media/deck/SN01T/sopr-fonttest/extracted"):
    if os.path.exists(os.path.join(d, "odyssey_game.so")): G = os.path.join(d, "odyssey_game.so")
want = sys.argv[2] if len(sys.argv) > 2 else None
rows = [l.split() for l in open("stacks.txt") if l[:1].isdigit()]
addrs = sorted({a.split("+")[1] for r in rows for a in r[1:] if a.startswith("SuperOdyssey")})
out = subprocess.run(["addr2line", "-f", "-C", "-e", B] + addrs, capture_output=True, text=True).stdout.split("\n")
name = {a: out[2 * i] for i, a in enumerate(addrs)}
gname = {}
if G:
    gaddrs = sorted({a.split("+")[1] for r in rows for a in r[1:] if a.startswith("odyssey_game")})
    gout = subprocess.run(["addr2line", "-f", "-e", G] + gaddrs, capture_output=True, text=True).stdout.split("\n")
    gname = {a: "game:" + gout[2 * i] for i, a in enumerate(gaddrs)}
def fns(r):
    return [name[a.split("+")[1]] if a.startswith("SuperOdyssey") else gname.get(a.split("+")[1], a.split("+")[0])
            if a.startswith("odyssey_game") else a.split("+")[0] for a in r[1:]]
incl, chains = collections.Counter(), collections.Counter()
tot = 0
for r in rows:
    c = int(r[0]); tot += c
    fs = fns(r)
    for f in set(fs): incl[f] += c
    if want and want in fs:
        i = fs.index(want)
        chains[" <- ".join(fs[i:i + 8])] += c
print("samples", tot)
for f, c in incl.most_common(45): print(c, f)
if want:
    print("--- chains through", want)
    for k, c in chains.most_common(15): print(c, k)
