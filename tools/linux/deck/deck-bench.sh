#!/bin/bash
# Copied from the Deck (~/sopr) into the repo 2026-11-15. Run on the Deck; env: OUT, APP (an AppImage or AppDir/AppRun), FULL=1
# (fullscreen), USER_INI, HANDHELD, EARLY, NOGMEM, HITCH_MS, GATE_MS, WARP, INPUT, SAMPLE_AT, SHOT, EXTRA_INI, PRESENT, IMAGES, SAVE, GAME_ENV, LAUNCH=steam, GS_SHOT, SHOTS.
# Data on the Deck: the 1.4.1 test copy /run/media/deck/SN01T/sopr-fonttest, ~/sopr/bench/savecomplete.tar (sopr-wintest save-complete).
# Deck bench: Cap Kingdom walk from the test copy, the game thread sampled (Linux sampler, SIGPROF) for 600 frames.
F=${DATA:-/run/media/deck/SN01T/sopr-fonttest}  # DATA=<folder>: another data folder (its own caches)
APP=${APP:-/home/deck/sopr/build/dist/Super Odyssey PC Recomp-0.2.10-linux-x86_64.AppImage}
OUT=${OUT:-$HOME/sopr/bench/run}
mkdir -p "$OUT"
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-0 DISPLAY=:0
export ODYSSEY_NO_CONFIG=1 ODYSSEY_DATA_ROOT=$F ODYSSEY_SAVE_DIR=$OUT/save ODYSSEY_WINDOW=1280x800 ODYSSEY_NO_UPDATE_CHECK=1
export ODYSSEY_PREPARE=0 ODYSSEY_SETTINGS=$OUT/set.ini ODYSSEY_VK_PROF=1 ODYSSEY_SYNC_EARLY=${EARLY:-} ODYSSEY_VK_NO_GMEM=${NOGMEM:-} ODYSSEY_HANDHELD=${HANDHELD:-} ODYSSEY_VK_PRESENT=${PRESENT:-mailbox} ODYSSEY_VK_IMAGES=${IMAGES:-} ODYSSEY_HITCH_MS=${HITCH_MS:-50} ODYSSEY_LOG_GATE_MS=${GATE_MS:-} ODYSSEY_LOG_SLOW_FRAMES=40 ODYSSEY_WARP=${WARP:-world:CapWorldHomeStage@1200}
export ODYSSEY_INPUT="${INPUT:-600:A:8,750:A:8,900:A:8,1050:A:8,2300:STICK_UP:240,2600:RSTICK_LEFT:150,2800:STICK_UP:300}"
[ "$FULL" = 1 ] && unset ODYSSEY_WINDOW ODYSSEY_HIDDEN
export ODYSSEY_LOG_PROGRAMS=${LOGP:-} ODYSSEY_SAMPLE_AT=${SAMPLE_AT:-2300} ODYSSEY_SAMPLE_FRAMES=${SAMPLE_FRAMES:-600} ODYSSEY_WINDOW_SHOT=${SHOT:-3300} ODYSSEY_WINDOW_SHOT_PATH=$OUT/shot.png ODYSSEY_EXIT_AFTER_SHOT=1
# SHOTS=<frame>,<frame>,...: the window saved at each of those frames too ($OUT/shot_<frame>.png): where a scripted
# route really went
if [ -n "${SHOTS:-}" ]; then
    h=""; for f in ${SHOTS//,/ }; do h="$h${h:+,}$f:shot:shot_$f"; done
    export ODYSSEY_HOST_INPUT="$h"
fi
# GAME_ENV=A=1,B=2: any other game settings (comma-separated)
[ -n "${GAME_ENV:-}" ] && export ${GAME_ENV//,/ }
# (a setting left empty above is dropped: the game may read "" as 0 / off instead of its default - early fences were
# off in every bench before 2026-11-15 unless EARLY=1 was given)
for v in $(env | grep -o '^ODYSSEY_[A-Z0-9_]*=$' | tr -d =); do unset "$v"; done
# SAVE=<a .tar or a save folder>: start from that save (copied; the original is never written)
rm -rf "$OUT/save"; mkdir -p "$OUT/save"
SAVE=${SAVE:-/home/deck/sopr/bench/savecomplete.tar}
if [ -d "$SAVE" ]; then cp -a "$SAVE/." "$OUT/save/"; else tar -xf "$SAVE" -C "$OUT/save/"; fi
[ -n "$USER_INI" ] && cp $USER_INI $OUT/set.ini || printf "frame_rate = 60
%b" "${EXTRA_INI:-}" > "$OUT/set.ini"
rm -f "$F/build/samples.txt" "$F/build/stacks.txt" "$F"/build/shot_*.png
D=$(dirname "$(readlink -f "$0")")
if [ "${LAUNCH:-}" = steam ]; then
    # Through Steam (Gaming Mode as the owner plays it): the Steam shortcut to play-dev.sh runs this with the settings
    # left in next-run.env (only the game's own; the display comes from Steam / gamescope)
    ID=$(python3 - <<'PY'
import glob, re, struct
for f in glob.glob('/home/deck/.steam/steam/userdata/*/config/shortcuts.vdf'):
    b = open(f, 'rb').read()
    for m in re.finditer(rb'\x02appid\x00(....)', b, re.I):
        e = re.search(rb'\x01exe\x00([^\x00]*)', b[m.end():m.end() + 2048], re.I)
        if e and b'play-dev.sh' in e.group(1):
            print((struct.unpack('<I', m.group(1))[0] << 32) | 0x02000000); raise SystemExit
PY
)
    [ -n "$ID" ] || { echo "no Steam shortcut to play-dev.sh: add it once (see play-dev.sh)" >> "$OUT/run.log"; echo DONE >> "$OUT/run.log"; exit 1; }
    rm -f "$OUT/game-exited" "$OUT/run.log"
    { echo "OUT=$OUT"; export -p | sed -n 's/^declare -x \(ODYSSEY_[A-Z0-9_]*=\)/export \1/p' | grep -v '^export ODYSSEY_WINDOW='; } > /home/deck/sopr/bench/next-run.env
    [ "$FULL" = 1 ] || echo "export ODYSSEY_WINDOW=1280x800" >> /home/deck/sopr/bench/next-run.env
    steam -ifrunning "steam://rungameid/$ID" > /dev/null 2>&1 &
    # GS_SHOT=<seconds>: also what gamescope shows (the composited screen, as the owner sees it) that long after start
    if [ -n "${GS_SHOT:-}" ]; then
        ( for i in $(seq 1 120); do [ -f "$OUT/run.log" ] && break; sleep 1; done; sleep "$GS_SHOT"
          gamescopectl screenshot "$OUT/gamescope.png" > /dev/null 2>&1 ) &
    fi
    for i in $(seq 1 960); do [ -f "$OUT/game-exited" ] && break; sleep 1; done
    [ -f "$OUT/game-exited" ] || echo "the game did not finish (or never started) through Steam" >> "$OUT/run.log"
else
    ("$D/deck-threads.sh" $OUT/threads.log &) ; ("$D/deck-gpu.sh" $OUT/gpu.log &) ; timeout 900 "$APP" > "$OUT/run.log" 2>&1
    echo "EXIT=$?" >> "$OUT/run.log"
fi
cp "$F/build/samples.txt" "$F/build/stacks.txt" "$F"/build/shot_*.png "$OUT/" 2>/dev/null
echo DONE >> "$OUT/run.log"
