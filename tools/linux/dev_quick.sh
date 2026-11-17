#!/bin/bash
# Fast iteration on Linux / the Steam Deck: rebuild only the runtime program (an incremental make, in the same
# container as tools/linux/appimage.sh) and put it into the AppDir the last full appimage.sh left in
# build/linux/x86_64/AppDir. Run that AppDir's AppRun directly - no AppImage. A runtime change takes seconds to a
# minute instead of the full build (generator helper, dependencies, packing: several minutes).
# Needs one full `tools/linux/appimage.sh x86_64` first. A change to the game code generator (tools/gen.py, lifter.py,
# runtime/cpu.h, simd*.h, gamelib.h) needs the full build: the game code is rebuilt on the device from those.
set -euo pipefail
cd "$(dirname "$0")/../.."
L=$PWD/build/linux/x86_64
A=$L/AppDir
[ -x "$A/AppRun" ] || { echo "no AppDir yet: run tools/linux/appimage.sh x86_64 once"; exit 1; }
IMG=localhost/superodysseyrecomp-build:amd64
podman image exists "$IMG" || { echo "no build image yet: run tools/linux/appimage.sh x86_64 once"; exit 1; }
t0=$(date +%s)
podman run --rm --arch amd64 -v "$PWD:/src" -w /src "$IMG" \
    make -j"$(nproc)" B=/src/build/linux/x86_64/game DEPS=/src/build/linux/x86_64/deps SHADERC=-lshaderc_shared ARCH=x86_64 \
    /src/build/linux/x86_64/game/SuperOdysseyRecomp
cp "$L/game/SuperOdysseyRecomp" "$A/usr/bin/SuperOdysseyRecomp"
mkdir -p "$A/usr/bin/lang" && cp runtime/ui/lang/*.txt "$A/usr/bin/lang/"  # (the menu's translations)
mkdir -p "$A/usr/bin/fsr2" && cp third_party/fsr2/shaders/* third_party/fsr2/LICENSE.txt "$A/usr/bin/fsr2/"  # (FSR 2)
# (as tools/linux/package.sh does: the AppDir's own libraries, through $ORIGIN/../lib)
podman run --rm --arch amd64 -v "$PWD:/src" -w /src "$IMG" \
    patchelf --set-rpath '$ORIGIN/../lib' /src/build/linux/x86_64/AppDir/usr/bin/SuperOdysseyRecomp
echo "ready in $(( $(date +%s) - t0 )) s: $A/AppRun"
