#!/bin/bash
# Odyssey test run on Windows (Git Bash): build, boot the game files in extracted/, stop it after
# SECS seconds (default 30) and show the end of the log. The whole log: logs/run.log.
#   SECS=60 tools/odyssey_run.sh        HIDDEN=1: no window        TAIL=80: more log lines
# Test runs never touch the player's save (save/save_0, the game may be running): they play on a copy,
# build/testsave/save_0, made fresh each run from TESTSAVE (default save/testsave, else none).
cd "$(dirname "$0")/.."
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p logs
make -j16 > logs/build.log 2>&1 || { grep -E 'error' -A3 logs/build.log | head -40; exit 1; }
[ -f extracted/romfs.bin ] || ./build/PowerMoonRecomp.exe --build-romfs extracted || exit 1
export ODYSSEY_NO_AUDIO=${ODYSSEY_NO_AUDIO:-1}
export ODYSSEY_SAVE_DIR=${ODYSSEY_SAVE_DIR:-build/testsave}
TESTSAVE=${TESTSAVE:-save/testsave}
rm -rf "$ODYSSEY_SAVE_DIR/save_0" && mkdir -p "$ODYSSEY_SAVE_DIR"
[ -d "$TESTSAVE" ] && cp -r "$TESTSAVE" "$ODYSSEY_SAVE_DIR/save_0"
[ -n "$HIDDEN" ] && export ODYSSEY_HIDDEN=1
./build/PowerMoonRecomp.exe extracted > logs/run.log 2>&1 &
PID=$!
for _ in $(seq 1 "${SECS:-30}"); do sleep 1; kill -0 $PID 2>/dev/null || break; done
if kill -0 $PID 2>/dev/null; then
    echo "(still running after ${SECS:-30}s, stopped)"
    kill -9 $PID 2>/dev/null
fi
wait $PID 2>/dev/null
echo "exit: $?"
grep -v '^\[svc\]' logs/run.log | tail -"${TAIL:-40}"
