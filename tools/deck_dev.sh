#!/bin/bash
# From the Windows PC (Git Bash): send this checkout's runtime / tools / Makefile to the Steam Deck's build checkout
# (~/sopr), rebuild only the runtime there (tools/linux/dev_quick.sh) and, with a bench name, run the scripted
# benchmark on it (tools/linux/deck/deck-bench.sh) and print its frame stats.
#
#   tools/deck_dev.sh                 sync + quick build
#   tools/deck_dev.sh bench <name>    ... + the busy Cap Kingdom walk; results in ~/sopr/bench/<name> on the Deck
#   tools/deck_dev.sh full            sync + the full AppImage (needed after generator changes, and once at first)
#
# Extra bench settings pass through the environment, e.g. FULL=1 USER_INI=/home/deck/sopr/bench/user.ini HANDHELD=1,
# and GAME_ENV=ODYSSEY_X=1,ODYSSEY_Y=2 for any other game setting. LAUNCH=steam: through the Steam shortcut to
# tools/linux/deck/play-dev.sh, i.e. in Gaming Mode as the owner plays it (add the shortcut once: see play-dev.sh).
# The Deck's address: DECK in tools/release.local.sh (git-ignored), e.g. DECK=deck@192.168.1.82.
set -euo pipefail
cd "$(dirname "$0")/.."
[ -f tools/release.local.sh ] && . tools/release.local.sh
: "${DECK:?set DECK=user@host in tools/release.local.sh}"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10"
MODE=${1:-quick}

# the sources, LF line endings (the working copy may be CRLF), modification times kept so make stays incremental
TAR=$(mktemp)
tar -cf "$TAR" runtime tools Makefile data/pipeline-seed.bin third_party/fsr2
timeout 300 scp -q -o BatchMode=yes "$(cygpath -m "$TAR" 2>/dev/null || echo "$TAR")" "$DECK:sopr-sync.tar"
rm -f "$TAR"
# (unpacked into a staging folder and made LF there; only files whose content differs are copied into ~/sopr, so
# make rebuilds just what changed)
$SSH "$DECK" 'set -e; rm -rf ~/sopr-stage && mkdir ~/sopr-stage && cd ~/sopr-stage && tar -xf ~/sopr-sync.tar && rm ~/sopr-sync.tar
    n=0; while IFS= read -r -d "" f; do
        case "$f" in *.c|*.h|*.mm|*.sh|*.py|*Makefile|*.txt|*.cpp|*.glsl|*.comp) sed -i "s/\r$//" "$f";; esac
        if ! cmp -s "$f" ~/sopr/"$f"; then mkdir -p ~/sopr/"$(dirname "$f")"; cp -p "$f" ~/sopr/"$f"; touch ~/sopr/"$f"; n=$((n+1)); fi
    done < <(find . -type f -print0)
    chmod +x ~/sopr/tools/linux/*.sh ~/sopr/tools/linux/deck/*.sh; cd ~ && rm -rf ~/sopr-stage; echo "$n files changed"'

if [ "$MODE" = full ]; then
    $SSH "$DECK" 'cd ~/sopr && bash tools/linux/appimage.sh x86_64 > build/appimage.log 2>&1; echo EXIT=$?; tail -1 build/appimage.log'
    exit 0
fi
# (a failed build stops here: a bench must never run the previous program)
$SSH "$DECK" 'cd ~/sopr && bash tools/linux/dev_quick.sh > build/dev_quick.log 2>&1; r=$?
    grep -E -A3 "error|warning: implicit" build/dev_quick.log | head -30; tail -1 build/dev_quick.log; exit $r' ||
    { echo "BUILD FAILED (log: ~/sopr/build/dev_quick.log on the Deck)"; exit 1; }

if [ "$MODE" = bench ]; then
    NAME=${2:?bench name}
    ENVS=""
    for v in FULL USER_INI HANDHELD EARLY NOGMEM HITCH_MS GATE_MS WARP INPUT SAMPLE_AT SAMPLE_FRAMES SHOT EXTRA_INI PRESENT IMAGES GAME_ENV SAVE LAUNCH GS_SHOT SHOTS DATA; do
        [ -n "${!v:-}" ] && ENVS="$ENVS --setenv=$v=${!v}"
    done
    $SSH "$DECK" "systemctl --user reset-failed sopr-bench 2>/dev/null; systemd-run --user --collect --unit=sopr-bench \
        --setenv=OUT=/home/deck/sopr/bench/$NAME --setenv=APP=/home/deck/sopr/build/linux/x86_64/AppDir/AppRun $ENVS \
        /home/deck/sopr/tools/linux/deck/deck-bench.sh >/dev/null && echo started"
    for i in $(seq 1 90); do
        sleep 10
        [ "$($SSH "$DECK" "grep -c DONE ~/sopr/bench/$NAME/run.log 2>/dev/null" || true)" = 1 ] && break
    done
    $SSH "$DECK" "L=/run/media/deck/SN01T/sopr-fonttest/SuperOdysseyRecomp.log; cp \$L ~/sopr/bench/$NAME/game.log;
        grep -ac '^\*\*\*\|FATAL' \$L | sed 's/^/errors: /';
        grep -a '^\[vk\] t=' \$L | sed -E 's/.*frame ([0-9]+) ([0-9.]+) fps max ([0-9]+)ms.*/f\1 \2 fps, worst \3 ms/' | tail -15"
fi
