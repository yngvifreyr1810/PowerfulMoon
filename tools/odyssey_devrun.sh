#!/bin/bash
# Test run next to the player's game (Git Bash): like tools/odyssey_run.sh, but the build goes to build/dev (a running
# build/PowerMoonRecomp.exe locks its exe and DLL) and the run has a data root of its own, build/devroot (extracted/ is a
# junction to the real one; own cache/ - the player's pipeline cache stays the NVIDIA one - own powermoon.ini, the save a
# fresh copy of TESTSAVE in build/devroot/testsave). GPU=1 (default): the AMD iGPU, so the player's game keeps the
# NVIDIA GPU; GPU=0 for performance. Screenshots (ODYSSEY_SHOT_AT) land in build/devroot/build/frames/.
#   SECS=60 HIDDEN=1 TAIL=80 GPU=1 CLEANFRAMES=1 tools/odyssey_devrun.sh
# First time: copy build/{obj,rt,imgui,inc,*.dll,*.id,PowerMoonRecomp.*} to build/dev with `cp -a` (timestamps kept: make
# rebuilds nothing), and the shader cache with `cp -a cache/spirv cache/drawn_programs.bin build/devroot/cache/`.
cd "$(dirname "$0")/.."
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p logs
nice -n 15 make -j16 B=build/dev > logs/devbuild.log 2>&1 || { grep -E 'error' -A3 logs/devbuild.log | head -40; exit 1; }
R=build/devroot
mkdir -p $R/cache $R/build/frames $R/testsave
[ -e $R/extracted ] || powershell -NoProfile -Command "New-Item -ItemType Junction -Path '$(cygpath -w $R/extracted)' -Target '$(cygpath -w extracted)'" >/dev/null
export ODYSSEY_DATA_ROOT="$(cygpath -w $R)"
export ODYSSEY_NO_AUDIO=${ODYSSEY_NO_AUDIO:-1}
export ODYSSEY_GPU=${GPU:-1}
export ODYSSEY_SAVE_DIR=testsave
TESTSAVE=${TESTSAVE:-save/testsave}
rm -rf "$R/testsave/save_0"
[ -d "$TESTSAVE" ] && cp -r "$TESTSAVE" "$R/testsave/save_0"
[ -n "$HIDDEN" ] && export ODYSSEY_HIDDEN=1
[ -n "$CLEANFRAMES" ] && rm -f "$R"/build/frames/*.png
./build/dev/PowerMoonRecomp.exe > logs/run.log 2>&1 &
PID=$!
for _ in $(seq 1 "${SECS:-30}"); do sleep 1; kill -0 $PID 2>/dev/null || break; done
if kill -0 $PID 2>/dev/null; then
    echo "(still running after ${SECS:-30}s, stopped)"
    kill -9 $PID 2>/dev/null
fi
wait $PID 2>/dev/null
echo "exit: $?"
grep -av '^\[svc\]' logs/run.log | tail -"${TAIL:-40}"
