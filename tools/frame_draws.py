"""Summarize the draws of one debug frame in a run log: per draw its program, targets, depth state, cull, polygon
offset; consecutive identical draws collapsed. Usage: frame_draws.py <log> [frame] [filter-substring]"""
import re
import sys

log = sys.argv[1]
frame = sys.argv[2] if len(sys.argv) > 2 else None
filt = sys.argv[3] if len(sys.argv) > 3 else None
lines = open(log, encoding='utf-8', errors='replace').read().splitlines()
start = 0
if frame:
    for i, l in enumerate(lines):
        if l.startswith('===== debug frame %s' % frame):
            start = i
            break
draws, cur = [], None
for l in lines[start:]:
    if l.startswith('===== debug frame') and cur is not None and frame and not l.endswith(frame + ' ====='):
        break
    m = re.match(r'\[draw\] ([ae]) inst (\d+) mode (\d+) count (\d+) prog vs=(\w+) fs=(\w+) \| vs cb \S+ fs cb \S+ \| rt (\d+)(.*?)\| vp .*attrs (\S+) blend (\S+) ds (\S+) poly (\S+) chmask (\S+)', l)
    if m:
        cur = dict(kind=m[1], inst=m[2], mode=m[3], count=m[4], vs=m[5], fs=m[6], rt=m[7] + m[8].strip(), attrs=m[9], blend=m[10],
                   ds=m[11], poly=m[12], chmask=m[13], extra='')
        draws.append(cur)
        continue
    m = re.match(r'\[draw\]   depth range (\S+) (\S+), poly mode (\S+) offset enables (\S+) \(([^)]*)\), attr0 (\S+) stride (\S+)', l)
    if m and cur:
        cur['extra'] = 'off %s(%s) a0 %s/%s' % (m[4], m[5], m[6], m[7])
    m = re.match(r'\[vk\] end of pass (\d+) \((\d+) draws\)', l)
    if m:
        draws.append(dict(endpass=m[1], n=m[2]))
prev, n = None, 0
out = []
for d in draws:
    if 'endpass' in d:
        key = '---- end of pass %s (%s draws)' % (d['endpass'], d['n'])
    else:
        if filt and filt not in d['rt']:
            continue
        key = '%s vs=%s fs=%s | %s | ds %s poly %s chm %s blend %s %s' % (d['kind'], d['vs'], d['fs'], d['rt'][:60], d['ds'], d['poly'],
                                                                      d['chmask'], d['blend'], d['extra'])
    if key == prev:
        n += 1
        continue
    if prev is not None:
        out.append('%3dx %s' % (n, prev))
    prev, n = key, 1
if prev is not None:
    out.append('%3dx %s' % (n, prev))
print('\n'.join(out))
