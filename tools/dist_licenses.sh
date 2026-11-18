#!/bin/bash
# usage: tools/dist_licenses.sh <dist folder>: the licences of everything the Windows release ships (make dist), in
# <dist>/licenses/, with README.txt naming each component, its licence and where its source is. Run from the repo root
# after make deps-windows and make gen-helper (it reads their build trees).
set -euo pipefail
D="$1/licenses"
# (the MSYS2 tree the build uses, wherever it is installed: its clang's ../share/licenses)
U=$(cd "$(dirname "$(command -v clang)")/../share/licenses" && pwd)
SP=build/gen-venv/Lib/site-packages
FF=build/deps-win/src/ffmpeg
PYHOME=$(build/gen-venv/Scripts/python.exe -c 'import sys; print(sys.base_prefix)')
mkdir -p "$D"
cp third_party/imgui/LICENSE.txt "$D/Dear-ImGui-MIT.txt"
cp third_party/smaa/LICENSE.txt "$D/SMAA-MIT.txt"
cp third_party/fsr/LICENSE.txt "$D/AMD-FSR1-MIT.txt"
cp third_party/ryujinx/LICENSE.txt "$D/Ryujinx-MIT.txt"
# MSYS2 packages' licences: the folder's name has changed between package versions (VulkanMemoryAllocator /
# vulkan-memory-allocator, ...), so each is found by pattern: the first match, or a clear error
lic() {  # out-name folder-pattern file-pattern
    local f
    f=$(find "$U" -mindepth 2 -maxdepth 3 -type f -ipath "$U/$2/$3" 2>/dev/null | sort | head -1)
    [ -n "$f" ] || { echo "tools/dist_licenses.sh: no $3 under $U/$2"; ls "$U"; exit 1; }
    cp "$f" "$D/$1"
}
lic VulkanMemoryAllocator-MIT.txt '*vulkan*memory*allocator*' 'LICENSE*'
lic SDL3-zlib.txt 'sdl3' 'LICENSE*'
lic zstd-BSD.txt 'zstd' 'LICENSE*'
lic Apache-2.0.txt 'shaderc' 'LICENSE*'
lic LLVM-Apache-2.0-with-LLVM-exception.txt 'llvm-libs' 'LICENSE*'
lic winpthreads.txt '*winpthread*' 'COPYING*'
lic GCC-runtime-library-exception.txt 'libgcc' 'COPYING.RUNTIME'
lic GPL-3.0.txt 'libgcc' 'COPYING3'
cp $FF/COPYING.LGPLv2.1 "$D/FFmpeg-LGPL-2.1.txt"
cp $FF/COPYING.GPLv2 "$D/GPL-2.0.txt"
lic libiconv-LGPL-2.1.txt 'libiconv' 'COPYING.LIB'
cp "$(cygpath -u "$PYHOME")/LICENSE.txt" "$D/Python-PSF.txt"
cp $SP/capstone-*.dist-info/LICENSE.TXT "$D/Capstone-BSD.txt"
cp $SP/lz4-*.dist-info/licenses/LICENSE "$D/python-lz4-BSD.txt"
cp build/zig/LICENSE "$D/Zig-MIT.txt"
cp build/upscalers/NVIDIA-RTX-SDK-LICENSE.txt "$D/NVIDIA-RTX-SDK-LICENSE.txt"
cp third_party/fsr3_sdk/LICENSE-SDK.md "$D/AMD-FSR-SDK-LICENSE.md"
cp third_party/xess/LICENSE-RUNTIME.txt "$D/Intel-XeSS-LICENSE.txt"
cp third_party/xess/third-party-programs.txt "$D/Intel-XeSS-THIRD-PARTY.txt"
cat > "$D/README.txt" <<'EOF'
Super Odyssey PC Recomp - third-party components in this release and their licences
==============================================================================

Super Odyssey PC Recomp itself: MIT License (LICENSE next to the program). It contains no Nintendo code, game files,
assets or fonts; the game code is built on your PC from your own copy of the game.

Program (SuperOdysseyRecomp.exe and the DLLs next to it)
- SDL 3 (zlib licence) - SDL3-zlib.txt - https://libsdl.org
- Zstandard (BSD licence, linked statically for shader cache compression) - zstd-BSD.txt - https://github.com/facebook/zstd
- FFmpeg 9.0.2, an LGPL-only build (avcodec, avformat, avutil, swscale; linked dynamically) - FFmpeg-LGPL-2.1.txt.
  Source: https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz, built unmodified with the configuration in
  tools/build_deps_windows.sh of the Super Odyssey PC Recomp source repository. You may replace these DLLs with your own
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

Upscaler runtimes (unmodified, as the vendors ship them and signed by them; each loaded only when chosen in the menu)
- nvngx_dlss.dll - NVIDIA DLSS 310.9.1, from https://github.com/NVIDIA/DLSS - NVIDIA-RTX-SDK-LICENSE.txt.
  This software contains source code provided by NVIDIA Corporation.
- amd_fidelityfx_loader_vk.dll, amd_fidelityfx_upscaler_vk.dll, amd_fidelityfx_framegeneration_vk.dll - AMD
  FidelityFX SDK 2.4.0 (FSR 3.1 / FSR 4, FSR frame generation), from
  https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK - AMD-FSR-SDK-LICENSE.md
- sl.*.dll, nvngx_dlssg.dll, NvLowLatencyVk.dll - NVIDIA Streamline 2.14.16 (DLSS frame generation, Reflex), from
  https://github.com/NVIDIAGameWorks/Streamline - NVIDIA-RTX-SDK-LICENSE.txt (headers: MIT)
- fsr2/ - AMD FidelityFX Super Resolution 2.2.1 shaders (GLSL), from
  https://github.com/GPUOpen-Effects/FidelityFX-FSR2 - MIT, fsr2/LICENSE.txt (run by the recomp's own code)
- libxess.dll - Intel XeSS 2.0.2, from https://github.com/intel/xess - Intel-XeSS-LICENSE.txt,
  Intel-XeSS-THIRD-PARTY.txt

Setup's tools (used once, on your PC, to build the game code from your own game files)
- zig/ - Zig 0.17.0 (MIT) - Zig-MIT.txt - https://ziglang.org - with LLVM / Clang / LLD (Apache 2.0 with LLVM
  exception) and MinGW-w64 headers
- odysseygen/ - the game code generator (MIT, this project's tools/gen.py, in odysseygen/tools) run by CPython 3.12,
  python.org's interpreter as it comes, signed by the Python Software Foundation (PSF licence) - Python-PSF.txt; with
  Capstone (BSD) - Capstone-BSD.txt; Unicorn 2.1.4 (GPL 2.0) - GPL-2.0.txt, source:
  https://github.com/unicorn-engine/unicorn/tree/2.1.4; python-lz4 (BSD) - python-lz4-BSD.txt
EOF
echo "licenses: $(ls "$D" | wc -l) files"
