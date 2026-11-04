"""Backward slice of a translated shader (build/glsl/*.glsl, ODYSSEY_SAVE_MSL=1): only the statements a target
depends on, in program order. Straight-line code only (the non-branchy translation), enough for vertex positions.
Usage: glsl_slice.py <file.glsl> [target regex, default gl_Position]"""
import re
import sys

path = sys.argv[1]
target = re.compile(sys.argv[2] if len(sys.argv) > 2 else r'gl_Position')
lines = open(path, encoding='utf-8').read().splitlines()

stmts = []  # (index, text, defined names, used names)
pc = None
block = None
for line in lines:
    s = line.strip()
    m = re.match(r'// pc ([0-9a-f]+): ([0-9a-f]+)', s)
    if m:
        pc = m.group(1) + ' ' + m.group(2)
        continue
    if block is not None:  # a { ... } block (xmad, texture lookups): one statement
        block.append(s)
        if s == '}' or s.endswith('}'):
            text = ' '.join(block)
            defs = set(re.findall(r'\b(r\d+|p\d+)\s*=(?!=)', text))
            used = set(re.findall(r'\b(r\d+|p\d+|a\d+|cb\d+)\b', text))
            stmts.append((len(stmts), pc, text, defs, used))
            block = None
        continue
    if s.startswith('{'):
        block = [s]
        if s.endswith('}'):
            text = s
            defs = set(re.findall(r'\b(r\d+|p\d+)\s*=(?!=)', text))
            used = set(re.findall(r'\b(r\d+|p\d+|a\d+|cb\d+)\b', text))
            stmts.append((len(stmts), pc, text, defs, used))
            block = None
        continue
    m = re.match(r'(r\d+|p\d+|gl_Position\[\d\]|v\d+\[\d\])\s*=\s*(.*);$', s)
    if m:
        lhs, rhs = m.group(1), m.group(2)
        used = set(re.findall(r'\b(r\d+|p\d+|a\d+|cb\d+)\b', rhs))
        stmts.append((len(stmts), pc, s, {lhs}, used))

need, keep = set(), []
for i, pcs, text, defs, used in reversed(stmts):
    hit = any(target.search(d) for d in defs) or (defs & need)
    if hit:
        keep.append((pcs, text))
        need -= {d for d in defs if not d.startswith('gl_')}
        need |= {u for u in used if u.startswith(('r', 'p'))}
for pcs, text in reversed(keep):
    print(f'{pcs or "":24} {text}')
print(f'-- {len(keep)} of {len(stmts)} statements; inputs still needed at entry: {sorted(need)}', file=sys.stderr)
