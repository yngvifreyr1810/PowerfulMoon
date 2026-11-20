#!/usr/bin/env python3
"""Stutter report from a SuperOdysseyRecomp.log recorded with the stutter switches on
(ODYSSEY_HITCH_MS, ODYSSEY_LOG_SLOW_FRAMES, ODYSSEY_LOG_SHADER_TIME, ODYSSEY_VK_PROF, ODYSSEY_LOG_STAGES - the
"Play and record.bat" launcher, or tools/linux/deck/play-dev.sh on the Steam Deck).

    python tools/stutter_report.py <log> [--top N]

Per area (kingdom / stage) entered: time spent, frame rate (2 s windows: lowest, 10th percentile, median), hitches over
25 / 50 / 100 ms, shader and pipeline builds that held a frame; then the worst hitches with their likely cause:
entering an area (the first ~2 s after it loads), a shader or pipeline build (a frame held for one), the GPU (the frame waited on it), the
display (present / acquire), or the game itself (game code, loading).
"""
import re
import sys
from collections import defaultdict

VK = re.compile(r'\[vk\] t=([\d.]+) frame (\d+) ([\d.]+) fps max (\d+)ms')
HITCH = re.compile(r'\[hitch\] frame (\d+): (\d+) ms = backend exec ([\d.]+) \(of which GPU waits ([\d.]+)\) \+ present ([\d.]+) '
                   r'\+ other ([\d.]+); .*?compiles (\d+) pending; present: GPU wait ([\d.]+), acquire ([\d.]+)')
SLOW = re.compile(r'\[slow\] frame (\d+): (\d+) ms')
SHTIME = re.compile(r'\[shtime\] (?:frame (\d+) held|draw waited ([\d.]+) ms)')
STAGE = re.compile(r'running at frame (\d+) stage "([A-Za-z0-9_]+)"')  # (the area the game's scene is in, as it changes)
SESSION = re.compile(r'\[session\] (.*)')


def cause(h, frame, entered, built):
    ms, ex, gw, pres, other, pend, pgw, acq = h
    if any(0 <= frame - e <= 120 for e in entered):
        return 'entering the area (loading)'
    if any(0 <= frame - b <= 1 for b in built):
        return 'shader/pipeline build'
    if other > ms * 0.5:
        return 'game code / loading (%.0f ms outside rendering)' % other
    if gw > ms * 0.5 or pgw > ms * 0.5:
        return 'GPU (waited %.0f ms)' % max(gw, pgw)
    if pres > ms * 0.5 or acq > ms * 0.5:
        return 'display (present %.0f ms)' % pres
    return 'rendering (%.0f ms building the frame)' % ex


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 20
    stage = '(start-up / menus)'
    frame_stage = []  # (frame, stage) as the log goes
    windows = defaultdict(list)
    hitches = []
    held = defaultdict(int)
    built = []  # frames held for a shader / pipeline build
    last_frame = 0
    session = ''
    with open(sys.argv[1], encoding='utf-8', errors='replace') as f:
        for line in f:
            if 'window closed' in line:  # (what follows is the game shutting down, not play)
                break
            m = SESSION.search(line)
            if m and not session:
                session = m.group(1)
            m = STAGE.search(line)
            if m and m.group(2) != stage:
                stage = m.group(2)
                frame_stage.append((int(m.group(1)), stage))
            m = VK.search(line)
            if m:
                last_frame = int(m.group(2))
                windows[stage].append((float(m.group(3)), int(m.group(4))))
                continue
            m = HITCH.search(line)
            if m:
                fr = int(m.group(1))
                vals = (int(m.group(2)), float(m.group(3)), float(m.group(4)), float(m.group(5)), float(m.group(6)),
                        int(m.group(7)), float(m.group(8)), float(m.group(9)))
                hitches.append((fr, stage, vals))
                last_frame = max(last_frame, fr)
                continue
            m = SHTIME.search(line)
            if m:
                held[stage] += 1
                built.append(int(m.group(1)) if m.group(1) else last_frame)
    print('Stutter report: %s' % (session or sys.argv[1]))
    print()
    order = []
    for _, s in [(0, '(start-up / menus)')] + frame_stage:
        if s not in order:
            order.append(s)
    print('%-28s %7s %7s %6s %6s %6s %6s %6s %7s' % ('area', 'time', 'lowest', '10%', 'median', '>25ms', '>50ms', '>100ms', 'builds'))
    for s in order:
        w = windows.get(s, [])
        hs = [h for h in hitches if h[1] == s]
        if not w and not hs:
            continue
        fps = sorted(x[0] for x in w)
        pct = lambda p: fps[min(len(fps) - 1, int(len(fps) * p))] if fps else 0
        print('%-28s %6.0fs %7.0f %6.0f %6.0f %6d %6d %6d %7d' % (
            s[:28], 2 * len(w), fps[0] if fps else 0, pct(0.1), pct(0.5),
            sum(h[2][0] > 25 for h in hs), sum(h[2][0] > 50 for h in hs), sum(h[2][0] > 100 for h in hs), held[s]))
    print()
    entered = [f for f, _ in frame_stage]
    worst = sorted(hitches, key=lambda h: -h[2][0])[:top]
    if not worst:
        print('No hitches logged (none over the ODYSSEY_HITCH_MS threshold).')
        return 0
    print('Worst hitches:')
    for fr, s, vals in worst:
        print('  frame %7d  %4d ms  %-24s %s' % (fr, vals[0], s[:24], cause(vals, fr, entered, built)))
    kinds = defaultdict(int)
    for fr, _, vals in hitches:
        if vals[0] > 50:
            kinds[cause(vals, fr, entered, built).split(' (')[0]] += 1
    if kinds:
        print()
        print('Hitches over 50 ms by likely cause: ' + ', '.join('%s %d' % (k, v) for k, v in sorted(kinds.items(), key=lambda kv: -kv[1])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
