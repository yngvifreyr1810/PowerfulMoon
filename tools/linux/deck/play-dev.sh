#!/bin/bash
# The Steam shortcut for testing on the Steam Deck (Gaming Mode or Desktop Mode): the newest quick build
# (tools/deck_dev.sh -> ~/sopr/build/linux/x86_64/AppDir) on the 1.4.1 TEST copy - never the real install.
# Add once in Desktop Mode: Steam > Games > Add a Non-Steam Game > Browse > /home/deck/sopr/tools/linux/deck/play-dev.sh
#
# Played from Steam: the owner's test save and settings, frame stats and thread CPU use logged to
#   /run/media/deck/SN01T/sopr-0.2.9.1/logs/dev-session-<time>.log (+ threads-<time>.log).
# Started by a scripted run from the PC (deck-bench.sh LAUNCH=steam): that run's settings are in
#   ~/sopr/bench/next-run.env (written just before; used once, and only if under 2 minutes old) and its output goes
#   to that run's folder.
APP=/home/deck/sopr/build/linux/x86_64/AppDir/AppRun
D=$(dirname "$(readlink -f "$0")")
N=/home/deck/sopr/bench/next-run.env
if [ -f "$N" ] && [ $(( $(date +%s) - $(stat -c %Y "$N") )) -lt 120 ]; then
    . "$N"
    mv "$N" "$OUT/run.env"
    ("$D/deck-threads.sh" "$OUT/threads.log" &) ; ("$D/deck-gpu.sh" "$OUT/gpu.log" &)
    timeout 900 "$APP" > "$OUT/run.log" 2>&1
    echo "EXIT=$?" >> "$OUT/run.log"
    touch "$OUT/game-exited"
    exit 0
fi
rm -f "$N"  # (a stale one: a scripted run that never started)

F=/run/media/deck/SN01T/sopr-fonttest
INI=$F/powermoon-0291.ini
# The 1.0.0 TEST copy instead while the marker file /run/media/deck/SN01T/play-1.0.0 exists (the same settings; its
# own save, $F/save - the Wooded Kingdom save of issue #3)
[ -e /run/media/deck/SN01T/play-1.0.0 ] && F=/run/media/deck/SN01T/sopr-100
L=/run/media/deck/SN01T/sopr-0.2.9.1/logs
TS=$(date +%Y%m%d-%H%M%S)
mkdir -p "$L"
export ODYSSEY_DATA_ROOT=$F ODYSSEY_SAVE_DIR=$F/save ODYSSEY_SETTINGS=$INI ODYSSEY_NO_UPDATE_CHECK=1
export ODYSSEY_VK_PROF=1 ODYSSEY_LOG_SLOW_FRAMES=25 ODYSSEY_LOG_STAGES=1 ODYSSEY_HITCH_MS=30 ODYSSEY_LOG_SHADER_TIME=5
# Every session is recorded so it can be replayed from the PC (ODYSSEY_INPUT_REPLAY): the input frame by frame
# ($L/ride-<time>.bin) and the save as it was at the start ($L/save-<time>). A save to show something with: its files
# in /run/media/deck/SN01T/put-your-save-here (used instead of the test save while that folder has files).
S=$F/save
P=/run/media/deck/SN01T/put-your-save-here
mkdir -p "$P"
[ -n "$(ls -A "$P" 2>/dev/null)" ] && S=$P
export ODYSSEY_SAVE_DIR=$S ODYSSEY_INPUT_RECORD=$L/ride-$TS.bin
cp -a "$S" "$L/save-$TS"
("$D/deck-threads.sh" "$L/threads-$TS.log" &) ; ("$D/deck-gpu.sh" "$L/gpu-$TS.log" &)
# (the log is kept by a user service once the game has exited: quitting from Steam ends this script, and everything it
# started, with the game - a detached helper too)
# (pgrep -x on the process name - SuperOdysseyRec, 15 characters: a -f pattern matched this helper's own command line,
# and it waited forever)
systemd-run --user --collect --quiet bash -c 'sleep 20; while pgrep -x SuperOdysseyRec > /dev/null; do sleep 2; done
    cp "$0" "$1"' "$F/SuperOdysseyRecomp.log" "$L/dev-session-$TS.log" > /dev/null 2>&1
"$APP" "$@"
