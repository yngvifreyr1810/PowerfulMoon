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
#      3b/3c. The zip's setup shader passes on a fresh cache: the pipeline seed must still build, and a new player's
#      play from that cache must translate (almost) no shaders itself.
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
#   SEEDDATA=X:/path    optional: the game files for steps 3b/3c (default TESTDATA), as setup makes them - with
#                       extracted/romfs.bin, which setup's shader pass scans (an old manual extraction has only romfs/)
#   DECK=user@host      optional: the Steam Deck over SSH (with ~/sopr/ai-case.sh and ~/sopr/deck-boot-ov.sh there)
set -euo pipefail
cd "$(dirname "$0")/.."

# This upstream publisher uses the upstream repository and its private catalog.
# PowerfulMoon validates and stages its own release artifacts instead.
if grep -q '^#define APP_UPDATE_REPO "yngvifreyr1810/PowerfulMoon"$' runtime/version.h; then
    echo 'PowerfulMoon releases follow docs/powerfulmoon-releases.md.' >&2
    exit 2
fi

VERSION=${1:?usage: tools/release.sh <version> <release-notes.md>}
NOTES=${2:?usage: tools/release.sh <version> <release-notes.md>}
[ -f "$NOTES" ] || { echo "no such file: $NOTES"; exit 1; }
[ -f tools/release.local.sh ] && . tools/release.local.sh
: "${TESTDATA:?set TESTDATA in tools/release.local.sh}"
SEEDDATA=${SEEDDATA:-$TESTDATA}
[ -f "$SEEDDATA/extracted/romfs.bin" ] || { echo "no $SEEDDATA/extracted/romfs.bin: set SEEDDATA (tools/release.local.sh) to game files set up by the setup"; exit 1; }
DECK=${DECK:-}
REPO=Simon3Sarkis/super-odyssey-pc-recomp
PROBE=Simon3Sarkis/sopr-catalog-probe
BRANCH=ci/release-$VERSION
WORK=$(cygpath -m "$(mktemp -d)")  # (C:/... form: gh and the other Windows programs get it as is)
SSH="/c/Windows/System32/OpenSSH/ssh.exe -o BatchMode=yes -o ConnectTimeout=10"
SCP="/c/Windows/System32/OpenSSH/scp.exe -q -o BatchMode=yes"
export MSYS_NO_PATHCONV=1  # (Git Bash would rewrite /tmp/... and /retest-style arguments)
say() { echo; echo "== $*"; }
# (the seed check's link to TESTDATA's game files - step 3b - is removed whatever happens: deleting the work folder
# through it would delete those files)
trap '[ -e "$WORK/seedcheck/extracted" ] && cmd /c rmdir "$(cygpath -w "$WORK/seedcheck/extracted")" > /dev/null 2>&1; echo "(work folder kept: $WORK)"' EXIT

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

say "3b/7  Windows: the shipped pipeline seed against this build's shaders"
# The setup compiles the pipelines in pipeline-seed.bin ahead. Its recipes name translated shaders by key, so a change
# to the shader translator can leave them pointing at nothing: 0.5.0 shipped a seed whose recipes were 63% unusable
# (4,318 of 6,905), and a new player's opening stuttered on what should have been ready. The zip's own setup passes are
# run here on a fresh cache (the game files linked from SEEDDATA) and the release stops if more than 5% are left out.
# Regenerate the seed with tools/make_pipeline_seed.py (see its docstring).
SEEDROOT="$WORK/seedcheck"
mkdir -p "$SEEDROOT/cache"
cmd /c mklink /J "$(cygpath -w "$SEEDROOT/extracted")" "$(cygpath -w "$SEEDDATA/extracted")" > /dev/null
( cd "$WORK/win/Super Odyssey PC Recomp" &&
  ./SuperOdysseyRecomp.exe --unpack-seed "$(cygpath -w "$SEEDROOT/cache/graphics-pipelines.bin")" > "$WORK/seed-unpack.log" 2>&1
  for pass in all recorded; do
      ODYSSEY_PRECOMPILE_MEM_MB=0 timeout 1800 ./SuperOdysseyRecomp.exe --prepare-shaders $pass --data-root "$(cygpath -w "$SEEDROOT")" \
          > "$WORK/seed-$pass.log" 2>&1
  done ) || true
SEEDLINE=$(grep -a "\[prep\] recorded " "$WORK/seed-recorded.log" | tail -1)
[ -n "$SEEDLINE" ] || { echo "the seed check's pipeline pass didn't run ($WORK/seed-recorded.log)"; exit 1; }
read -r SG SC SU <<< "$(echo "$SEEDLINE" | sed -E 's/.*recorded ([0-9]+) graphics and ([0-9]+) compute pipelines, ([0-9]+) unavailable.*/\1 \2 \3/')"
echo "$SEEDLINE"
if [ $((SU * 100)) -gt $(((SG + SC + SU) * 5)) ]; then
    echo "the pipeline seed is out of date: $SU of $((SG + SC + SU)) recipes can't be built by this build's setup."
    echo "Regenerate data/pipeline-seed.bin.zst (tools/make_pipeline_seed.py) - not releasing."
    exit 1
fi
echo "ok: $SU of $((SG + SC + SU)) left out"

say "3c/7  Windows: a new player's play translates no shaders"
# The setup above translates every program in the game files (~31,500) and saves them; the game must then take each
# from there. Played here from that fresh setup's cache: TESTDATA's Cap Kingdom save, or RIDE (a recorded ride,
# tools/release.local.sh: RIDE=file.bin RIDESAVE=its save-at-start folder) for a longer route. The game's census at the
# quit counts the programs it had to translate itself; the release stops above 50 (2026-11-19: 10 on a 3-minute ride,
# was 12,646 - each ~4.6 ms of a worker's time with its draws skipped meanwhile).
cp -r "${RIDESAVE:-$TESTDATA/save-cap}" "$WORK/save-play"
( cd "$WORK/win/Super Odyssey PC Recomp" &&
  if [ -n "${RIDE:-}" ]; then export ODYSSEY_INPUT_REPLAY="$(cygpath -w "$RIDE")"; fi &&
  ODYSSEY_DATA_ROOT="$(cygpath -w "$SEEDROOT")" ODYSSEY_SAVE_DIR="$(cygpath -w "$WORK/save-play")" ODYSSEY_PREPARE=0 \
  ODYSSEY_NO_CONFIG=1 ODYSSEY_NO_UPDATE_CHECK=1 ODYSSEY_DISCORD=0 ODYSSEY_WINDOW=1280x720 \
  ODYSSEY_INPUT='1500:A:8,1650:A:8,1800:A:8,1950:A:8,2100:A:8' ODYSSEY_HOST_INPUT="${PLAY_FRAMES:-9000}:quit:x" timeout 1800 ./SuperOdysseyRecomp.exe > "$WORK/play.log" 2>&1 ) || true
cmd /c rmdir "$(cygpath -w "$SEEDROOT/extracted")" > /dev/null 2>&1 || true
CENSUS=$(grep -a "\[census\] translated" "$WORK/play.log" | tail -1)
[ -n "$CENSUS" ] || { echo "the play run gave no census ($WORK/play.log)"; exit 1; }
echo "$CENSUS"; grep -a "\[census\] compiled" "$WORK/play.log" | tail -1
TN=$(echo "$CENSUS" | sed -E 's/.*not prepared ([0-9]+).*/\1/')
[ "$TN" -le 50 ] || { echo "the game translated $TN shaders a setup should have - not releasing ($WORK/play.log)"; exit 1; }
echo ok

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
    timeout 60 $SCP "$DECK:/run/media/deck/SN01T/sopr-fonttest/build/window.png" "$(cygpath -m "$WORK/deck.png")" || true
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
