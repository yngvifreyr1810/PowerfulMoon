# Sources, authors and redistribution

PowerfulMoon is a customized fork, not an original creation of the upstream port.

* **Power Moon Recomp**, zzzarathustra-dev and contributors, MIT:
  https://gitlab.com/zzzarathustra-dev/power-moon-recomp
  Integrated through v0.2.1, commit cdbe05c9df0e2ff4bfbecece89ad3f0fd9806193.
  The upstream 0.2.1 source also supplies shader preparation and render-queue improvements.
  Upstream authored the 0.2.0 renderer/interpolation/ultrawide changes; this fork adds
  controller persistence, VRR output selection and the host VSync setting.
  Original host, renderer, recompiler, setup flow, UI and original crescent icon
  are retained under MIT. Game-rendered screenshots have been excluded.
* **LAS Recomp**, Peiikai and contributors, MIT:
  https://github.com/peiikai/LASRecomp — upstream foundation. Original copyright
  and MIT grant remain in LICENSE. Additional upstream acknowledgements and
  all library sources are retained in UPSTREAM.md and third_party/.
* **Switchbrew / libnx**, switchbrew contributors, ISC:
  https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/hid.h
  and https://switchbrew.org/wiki/HID_services — published ABI and HID IPC
  layouts used as implementation references; no Nintendo SDK code is included.
* **SDL**, SDL contributors, zlib:
  https://github.com/libsdl-org/SDL — input/sensor events and the fallback rumble API.
* **totk-pc-recomp**, valentderah and contributors:
  https://github.com/valentderah/totk-pc-recomp/blob/main/runtime/input.c
  Axis/unit conventions were cross-checked against its source; none of that
  source or any game code is copied into this fork's controller implementation.
* **ProCon2Bridge**, Vĩnh An Project, GPL-3.0-or-later:
  https://github.com/yngvifreyr1810/ProCon2Bridge — separately distributed process,
  accessed through a public local UDP protocol. Its GPL BLE/vendor implementation
  is not included or linked into this MIT executable. See its THIRD_PARTY.md for
  Switch2Connect, pad2go, Switch2-pico and other protocol research credits.
* **LLVM libc++**, LLVM contributors, Apache-2.0 WITH LLVM-exception:
  https://github.com/llvm/llvm-project/tree/main/libcxx
  The Windows update adds the unmodified MSYS2 CLANG64 libc++.dll and the full
  accompanying license. MSYS2 recipe/source: https://github.com/msys2/MINGW-packages/tree/master/mingw-w64-libc%2B%2B
  Other dependencies are provided by the user's upstream installation, not bundled
  again in this update. Their licenses remain in the original installation.

New controller adapter, transport, tests, installer and documentation were developed
for **Vĩnh An Project / yngvifreyr1810**, with OpenAI Codex assistance and testing
by the project owner on their controller. They are offered under MIT with the
retained upstream notices. No AI-generated game art or external game assets are used.

Only source that can be redistributed under the respective licenses is published.
No game data, generated game-code library, decryption keys, console firmware,
Nintendo font, screenshot or proprietary SDK is part of this repository/release.


## Full distribution

- Original crescent artwork: zzzarathustra-dev / Power Moon Recomp, MIT. Retained unchanged. No Cappy/Nintendo image is included.
- Full host and installer customization: Vĩnh An Project / yngvifreyr1810, with OpenAI Codex implementation assistance, MIT.
- Bundled ProCon2Bridge 0.4.0: separate GPL-3.0-or-later program; its complete project source, dependency notices and signed third-party USB/IP installer remain in controller-bridge/.
- Windows runtime and setup dependencies are pinned by tools/windows/redistributables-0.2.0.json. The original licenses/README.txt identifies each component, license and source. FFmpeg DLLs were checked to have neither --enable-gpl nor --enable-nonfree.
- Corresponding FFmpeg 9.0.2, libiconv 1.19, Unicorn 2.1.4 and pystray 0.19.5 source archives are included under licenses/source/. Their upstream URLs and SHA-256 values are pinned in tools/windows/dependency-sources.json. These are library sources, not game content.
- CPython's interpreter and standard library, Capstone, python-lz4, Zig/LLVM/MinGW, SDL, shaderc, libc++, libiconv, winpthreads, ImGui, VMA, SMAA, FSR1, Ryujinx-derived upstream portions and the non-Nintendo Roboto font retain their original notices. No proprietary game or SDK material is used for the program's branding.
