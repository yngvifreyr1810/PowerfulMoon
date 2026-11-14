#!/bin/bash
# Release pipeline: bumps the version, has GitHub build both packages from a clean checkout (.github/workflows/build.yml),
# tests exactly those packages, and only then publishes them. Nothing reaches main or a release until all of it passes.
#
# Usage: tools/release.sh <version> <path-to-release-notes.md>     (Git Bash on Windows, gh signed in)
#
#   1. Bump runtime/version.h and runtime/platform/windows.rc; commit locally.
#   2. Push that commit to ci/release-<version> (not main): GitHub builds the Windows zip and the Linux AppImage on
#      clean machines. Download both.
#   3. Windows: run the downloaded zip's exe on a copied save until it is playing in Cap Kingdom (a first run after a
#      change to the game code's sources rebuilds that code first, as players' setups do) and take a screenshot.
#   4. Linux: if the Steam Deck (DECK) answers, start the AppImage there with no Vulkan driver and normally - each must
#      show a window - and play it for 2 minutes from the installed save. Skipped with a warning when it doesn't.
#   5. The AppImage catalog's own test (the private probe repository) on the downloaded AppImage, before publishing.
#   6. Only if all passed: push the commit to main and create the GitHub release with both packages.
#   7. The probe again on the published file; the ci/ branch and the staging build are removed.
#
# Look at the screenshots it names before telling anyone: it checks "runs and shows a window", not "looks right".
#
# This machine's own places: tools/release.local.sh (git-ignored) sets
#   TESTDATA=X:/path    a game data root (ODYSSEY_DATA_ROOT) with extracted game files and a Cap Kingdom save in save-cap/
#   DECK=user@host      optional: the Steam Deck over SSH (with ~/sopr/ai-case.sh and ~/sopr/deck-boot-ov.sh there)
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=${1:?usage: tools/release.sh <version> <release-notes.md>}
NOTES=${2:?usage: tools/release.sh <version> <release-notes.md>}
[ -f "$NOTES" ] || { echo "no such file: $NOTES"; exit 1; }
[ -f tools/release.local.sh ] && . tools/release.local.sh
: "${TESTDATA:?set TESTDATA in tools/release.local.sh}"
DECK=${DECK:-}
REPO=Simon3Sarkis/super-odyssey-pc-recomp
PROBE=Simon3Sarkis/sopr-catalog-probe
BRANCH=ci/release-$VERSION
WORK=$(cygpath -m "$(mktemp -d)")  # (C:/... form: gh and the other Windows programs get it as is)
SSH="/c/Windows/System32/OpenSSH/ssh.exe -o BatchMode=yes -o ConnectTimeout=10"
SCP="/c/Windows/System32/OpenSSH/scp.exe -q -o BatchMode=yes"
export MSYS_NO_PATHCONV=1  # (Git Bash would rewrite /tmp/... and /retest-style arguments)
say() { echo; echo "== $*"; }
trap 'echo "(work folder kept: $WORK)"' EXIT

probe() {  # url -> runs the catalog probe on it, fails unless both of its checks find the window
    gh workflow run probe.yml --repo "$PROBE" -f "url=$1" >/dev/null
    sleep 12
    local id; id=$(gh run list --repo "$PROBE" --workflow probe.yml --limit 1 --json databaseId -q '.[0].databaseId')
    gh run watch "$id" --repo "$PROBE" --exit-status >/dev/null
}

say "1/7  version $VERSION"
git diff --quiet HEAD -- . ':!runtime/version.h' ':!runtime/platform/windows.rc' || { echo "commit or stash your changes first"; exit 1; }
sed -i "s/#define APP_VERSION \".*\"/#define APP_VERSION \"$VERSION\"/" runtime/version.h
sed -i "s/VALUE \"FileVersion\", \".*\"/VALUE \"FileVersion\", \"$VERSION\"/;
        s/VALUE \"ProductVersion\", \".*\"/VALUE \"ProductVersion\", \"$VERSION\"/" runtime/platform/windows.rc
git diff --quiet || git commit -qam "Super Odyssey PC Recomp $VERSION"  # (a rerun: already bumped)
SHA=$(git rev-parse HEAD)

say "2/7  GitHub builds both packages ($BRANCH)"
git push -q origin "HEAD:refs/heads/$BRANCH"
RUN=
for i in $(seq 1 30); do
    RUN=$(gh run list --repo "$REPO" --workflow build.yml --commit "$SHA" --limit 1 --json databaseId -q '.[0].databaseId')
    [ -n "$RUN" ] && break; sleep 5
done
[ -n "$RUN" ] || { echo "no build run for $SHA"; exit 1; }
gh run watch "$RUN" --repo "$REPO" --exit-status >/dev/null || { echo "the CI build failed: gh run view $RUN --repo $REPO --log-failed"; exit 1; }
gh run download "$RUN" --repo "$REPO" -D "$WORK"
ZIP=$(ls "$WORK"/windows-zip/*-"$VERSION"-windows.zip)
APP=$(ls "$WORK"/linux-appimage/*-"$VERSION"-linux-x86_64.AppImage)
echo "built: $(basename "$ZIP"), $(basename "$APP")"

say "3/7  Windows: the zip's exe, playing in Cap Kingdom"
mkdir -p "$WORK/win" && /c/Windows/System32/tar.exe -xf "$ZIP" -C "$WORK/win"
( cd "$WORK/win/Super Odyssey PC Recomp" &&
  ODYSSEY_DATA_ROOT="$TESTDATA" ODYSSEY_SAVE_DIR="$TESTDATA/save-cap" ODYSSEY_PREPARE=0 ODYSSEY_NO_UPDATE_CHECK=1 \
  ODYSSEY_WINDOW=1280x720 ODYSSEY_INPUT='1500:A:8,1650:A:8,1800:A:8,1950:A:8,2100:A:8' ODYSSEY_WINDOW_SHOT=2700 \
  ODYSSEY_WINDOW_SHOT_PATH="$(cygpath -m "$WORK/windows-play.png")" ODYSSEY_EXIT_AFTER_SHOT=1 \
  timeout 2400 ./SuperOdysseyRecomp.exe > "$WORK/windows.log" 2>&1 ) || true
grep -aE "FATAL|\*\*\* " "$WORK/windows.log" && { echo "the Windows run hit an error ($WORK/windows.log)"; exit 1; }
[ -f "$WORK/windows-play.png" ] || { echo "the Windows run never reached gameplay ($WORK/windows.log)"; exit 1; }
echo "ok: $(grep -a fps "$WORK/windows.log" | tail -1 | cut -c1-50); look at $WORK/windows-play.png"

say "4/7  Linux on the Steam Deck"
if [ -n "$DECK" ] && timeout 15 $SSH "$DECK" true 2>/dev/null; then
    # (one fixed name the Deck's scripts use: ~/sopr/ai-case.sh, ~/sopr/deck-boot-ov.sh - its test data folder, never
    # the real install; a game code rebuild there takes ~20 min before its 2 minutes of play)
    timeout 300 $SCP "$APP" "$DECK:sopr/build/release-check.AppImage"
    timeout 3400 $SSH "$DECK" "
        cd ~/sopr && chmod +x build/release-check.AppImage
        bash ai-case.sh nogpu 2>&1 | grep '=='; bash ai-case.sh normal 2>&1 | grep '=='
        systemctl --user reset-failed sopr-boot 2>/dev/null; rm -f build/boot-test.log
        systemd-run --user --collect --unit=sopr-boot /home/deck/sopr/deck-boot-ov.sh >/dev/null 2>&1
        for i in \$(seq 1 660); do grep -q DONE build/boot-test.log 2>/dev/null && break; sleep 5; done
        cat build/boot-test.log" | tee "$WORK/deck.log"
    grep -q "nogpu: window: *0x" "$WORK/deck.log" || { echo "no window on the Deck without a Vulkan driver"; exit 1; }
    grep -q "normal: window: *0x" "$WORK/deck.log" || { echo "no window on the Deck at a first start"; exit 1; }
    grep -q "still running after 120" "$WORK/deck.log" || { echo "the Deck's 2-minute run ended early"; exit 1; }
    grep -q "errors in the log: 0" "$WORK/deck.log" || { echo "the Deck's run logged an error"; exit 1; }
    timeout 60 $SCP "$DECK:/run/media/deck/SN01T/Super Odyssey/game/build/window.png" "$(cygpath -m "$WORK/deck.png")" || true
    echo "ok; look at $WORK/deck.png"
else
    echo "WARNING: the Steam Deck isn't reachable - the Linux build is covered only by the catalog probe below"
fi

say "5/7  the AppImage catalog's test, before publishing"
gh release delete staging --repo "$PROBE" -y --cleanup-tag 2>/dev/null || true
gh release create staging --repo "$PROBE" --prerelease --title staging --notes "release.sh staging build, not a release" "$APP" >/dev/null
probe "https://github.com/$PROBE/releases/download/staging/$(basename "$APP" | tr ' ' '.')" ||
    { echo "the catalog probe failed on the build - not releasing"; exit 1; }
echo ok

say "6/7  publishing v$VERSION"
git push -q origin "$SHA:refs/heads/main"
gh release create "v$VERSION" "$ZIP" "$APP" --repo "$REPO" --target "$SHA" \
    --title "Super Odyssey PC Recomp $VERSION (preview)" --notes-file "$NOTES"

say "7/7  the catalog's test on the published file"
if probe "https://github.com/$REPO/releases/download/v$VERSION/$(basename "$APP" | tr ' ' '.')"; then
    echo "RELEASED and confirmed: https://github.com/$REPO/releases/tag/v$VERSION"
else
    echo "RELEASED, but the probe failed on the published file - look into it before a catalog /retest"
fi
gh release delete staging --repo "$PROBE" -y --cleanup-tag 2>/dev/null || true
git push -q origin --delete "$BRANCH" 2>/dev/null || true
