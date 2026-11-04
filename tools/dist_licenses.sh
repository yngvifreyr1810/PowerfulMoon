#!/bin/bash
# usage: tools/dist_licenses.sh <dist folder>: the licences of everything the Windows release ships (make dist), in
# <dist>/licenses/, with README.txt naming each component, its licence and where its source is. Run from the repo root
# after make deps-windows and make gen-helper (it reads their build trees).
set -euo pipefail
D="$1/licenses"
U=/c/msys64/ucrt64/share/licenses
SP=build/gen-venv/Lib/site-packages
FF=build/deps-win/src/ffmpeg
PYHOME=$(build/gen-venv/Scripts/python.exe -c 'import sys; print(sys.base_prefix)')
mkdir -p "$D"
cp third_party/imgui/LICENSE.txt "$D/Dear-ImGui-MIT.txt"
cp third_party/smaa/LICENSE.txt "$D/SMAA-MIT.txt"
cp third_party/fsr/LICENSE.txt "$D/AMD-FSR1-MIT.txt"
cp third_party/ryujinx/LICENSE.txt "$D/Ryujinx-MIT.txt"
cp $U/VulkanMemoryAllocator/LICENSE "$D/VulkanMemoryAllocator-MIT.txt"
cp $U/sdl3/LICENSE.txt "$D/SDL3-zlib.txt"
cp $U/shaderc/LICENSE "$D/Apache-2.0.txt"
cp $U/llvm-libs/LICENSE "$D/LLVM-Apache-2.0-with-LLVM-exception.txt"
cp $U/libwinpthread/COPYING "$D/winpthreads.txt"
cp $U/libgcc/COPYING.RUNTIME "$D/GCC-runtime-library-exception.txt"
cp $U/libgcc/COPYING3 "$D/GPL-3.0.txt"
cp $FF/COPYING.LGPLv2.1 "$D/FFmpeg-LGPL-2.1.txt"
cp $FF/COPYING.GPLv2 "$D/GPL-2.0.txt"
cp $U/libiconv/COPYING.LIB "$D/libiconv-LGPL-2.1.txt"
cp "$(cygpath -u "$PYHOME")/LICENSE.txt" "$D/Python-PSF.txt"
cp $SP/capstone-*.dist-info/LICENSE.TXT "$D/Capstone-BSD.txt"
cp $SP/lz4-*.dist-info/licenses/LICENSE "$D/python-lz4-BSD.txt"
cp build/zig/LICENSE "$D/Zig-MIT.txt"
cat > "$D/README.txt" <<'EOF'
Power Moon Recomp - third-party components in this release and their licences
==============================================================================

Power Moon Recomp itself: MIT License (LICENSE next to the program). It contains no Nintendo code, game files,
assets or fonts; the game code is built on your PC from your own copy of the game.

Program (PowerMoonRecomp.exe and the DLLs next to it)
- SDL 3 (zlib licence) - SDL3-zlib.txt - https://libsdl.org
- FFmpeg 9.0.2, an LGPL-only build (avcodec, avformat, avutil, swscale; linked dynamically) - FFmpeg-LGPL-2.1.txt.
  Source: https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz, built unmodified with the configuration in
  tools/build_deps_windows.sh of the Power Moon Recomp source repository. You may replace these DLLs with your own
  build of the same FFmpeg version.
- shaderc (Apache 2.0) - Apache-2.0.txt - https://github.com/google/shaderc
- GNU libiconv 1.19 (LGPL 2.1, used by SDL 3; linked dynamically) - libiconv-LGPL-2.1.txt -
  source: https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.19.tar.gz
- LLVM's libc++ / libunwind runtime (Apache 2.0 with LLVM exception) - LLVM-Apache-2.0-with-LLVM-exception.txt
- MinGW-w64 winpthreads - winpthreads.txt; GCC runtime library (GPL 3.0 with the GCC runtime library exception) -
  GCC-runtime-library-exception.txt, GPL-3.0.txt
- Dear ImGui (MIT) - Dear-ImGui-MIT.txt; Vulkan Memory Allocator (MIT) - VulkanMemoryAllocator-MIT.txt
- SMAA (MIT) - SMAA-MIT.txt; AMD FidelityFX Super Resolution 1 (MIT) - AMD-FSR1-MIT.txt
- Parts of the audio renderer and of the shader translator follow an MIT-licensed project - Ryujinx-MIT.txt
- The Roboto font, Roboto-Medium.ttf (Apache 2.0, Google) - Apache-2.0.txt

Setup's tools (used once, on your PC, to build the game code from your own game files)
- zig/ - Zig 0.17.0 (MIT) - Zig-MIT.txt - https://ziglang.org - with LLVM / Clang / LLD (Apache 2.0 with LLVM
  exception) and MinGW-w64 headers
- odysseygen/ - the game code generator (MIT, this project's tools/gen.py, in odysseygen/tools) run by CPython 3.12,
  python.org's interpreter as it comes, signed by the Python Software Foundation (PSF licence) - Python-PSF.txt; with
  Capstone (BSD) - Capstone-BSD.txt; Unicorn 2.1.4 (GPL 2.0) - GPL-2.0.txt, source:
  https://github.com/unicorn-engine/unicorn/tree/2.1.4; python-lz4 (BSD) - python-lz4-BSD.txt
EOF
echo "licenses: $(ls "$D" | wc -l) files"
