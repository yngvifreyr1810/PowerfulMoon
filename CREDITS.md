# Sources, authors and redistribution

This is a controller-focused fork, not an original creation of the upstream port.

* **Power Moon Recomp**, zzzarathustra-dev and contributors, MIT:
  https://gitlab.com/zzzarathustra-dev/power-moon-recomp
  Based on v0.1.6, commit 2957896536727cdff26598bdd007733e65c6af89.
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
