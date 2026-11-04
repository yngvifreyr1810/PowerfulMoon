# Controller implementation and validation

New code: runtime/controller_motion.*, motion_math.h, hd_transport.*, hd_wire.h,
runtime/hle/controller_motion.c. Integration: input events/frame, HID IPC
201/202/203/204/205/206, settings, and the existing ImGui Controls page.

The motion adapter rotates SDL axes into nn::hid coordinates, uses g and rev/s,
integrates a unit quaternion, gently corrects roll/pitch near 1 G, retains 64
samples (reads return at most 17), and drops motion older than 200 ms. This is
an approximation of sensor fusion, not Nintendo's firmware algorithm. One
full-size controller/player is supported by these new motion hooks.

Vibration retains left/right low/high amplitude/frequency independently over
the loopback [ProCon2Bridge v1 protocol](https://github.com/yngvifreyr1810/ProCon2Bridge/blob/main/docs/HD-RUMBLE.md).
The bridge's matching serial identity and a live connected capability reply are
required. Source updates are mutex-serialized; socket I/O is nonblocking; SDL
calls remain on the owner thread. When no compatible bridge is available, use
SDL two-motor fallback. Stale game parameters expire after 500 ms and the direct
transport lease expires after 200 ms. Test vibration overrides gameplay for
2 seconds and then returns to the most recent game effect.

## Verified / unverified

* Motion axes, static gravity, quaternion 90-degree rotation, ABI sizes, gaps,
  finite/clamped vibration values: tests/test_motion.c passed locally.
* Direct wire serialization: tests/test_hd_wire.c passed; its binary fixture was
  decoded by the Python bridge tests. Bridge tests also cover per-band/per-side
  output, temporal subframes, TTL, invalid data, replay and source ownership.
* Native Windows build linked successfully. Existing 0.2.0 generated game library
  loaded unmodified on the test installation. Actual game logs recorded sensor
  reads, vibration requests and `direct-haptics=active`.
* User tested motion in Odyssey gameplay, event-responsive rumble and the sustained
  two-second button test; reported the direct profile somewhat better than before.
* No measured actuator frequency/amplitude response, console parity, multiple-player
  motion, NFC/audio, Linux or macOS hardware validation. Windows automatic UI
  automation was unavailable; the user provided the in-game visual and physical checks.

## Build

Use MSYS2 **CLANG64** with clang, make, SDL3 headers/import libraries, FFmpeg,
Vulkan headers/loader, shaderc and VMA. See docs/BUILD-windows.md for upstream
dependencies. From a CLANG64 shell:

```
make -j12 DEPS=/clang64 build/PowerfulMoon.exe
clang tests/test_motion.c -lm -o build/test_motion.exe
clang tests/test_hd_wire.c -o build/test_hd_wire.exe
build/test_motion.exe
build/test_hd_wire.exe
```

The game-library ID is computed automatically for the renamed PowerfulMoon host. Do not change
generated code or the SDK symbol inventory for this update. No game data is
needed to build the host or these tests. Release compiler: MSYS2 CLANG64; SDL3
headers 3.4.18, FFmpeg headers 9.0.2, Vulkan 1.4.363, shaderc 2026.4, VMA 3.5.
Release includes libc++.dll; its source and license are linked in CREDITS.md.


## 0.2.0 migration and display policy (before the PowerfulMoon rename)

The complete upstream runtime delta v0.1.6 -> v0.2.0 is retained, with controller
hooks merged into settings and UI. All generator/ABI inputs match v0.2.0. The
Windows release uses ID 93fc548727b32ca9 and includes the corresponding host import
library and generator patch; no generated game-code library is packaged.

The controller sidecar atomically saves toggles, strength and complete bindings.
Tests simulate a stock settings rewrite, recover controller preferences and keep
new upstream video settings. Installer tests cover nested program resources,
settings/save preservation, repeat installation, stock-host reapplication,
restoration and rejection of unknown versions. The local game loaded its existing
0.2.0 library; the user confirmed motion/rumble working.

The original interpolation gate checked only SDL's current refresh, and the local
log disabled interpolation at 60 Hz despite a 2560x1440 display supporting 180 Hz.
The explicit 120 fps (VRR) mode verifies a supported >=119 Hz mode at the same
resolution, then permits interpolation independently of a transient 60 Hz reading.
Auto uses current/desktop nominal refresh. No physical VRR-state API is claimed;
this is an explicit user output policy. Pure tests cover 60 -> 120 on capable
hardware, actual 60 Hz-only displays and 119.99 Hz modes. Simulation remains 60 Hz.


## PowerfulMoon full package

The host is now PowerfulMoon.exe, with PowerfulMoon.dll.a for its imports. runtime/app_identity.h is
part of the game-library identity, so an old game-code cache is rebuilt locally rather than incorrectly
loaded against a renamed PE module. Leave powermoon.ini/profile names unchanged for safe migration.

Build the host using the Makefile on MSYS2 CLANG64, then strip its debug sections. Full Windows setup:

```
make -j12 DEPS=/path/to/clang64 build/PowerfulMoon.exe
llvm-strip --strip-debug build/PowerfulMoon.exe
powershell -File tools/windows/build_full.ps1 -RuntimeRoot <clean-upstream-0.2.0-folder> -BridgeZip <ProCon2Bridge-0.4.0-windows-x64.zip> -Libcxx <redistributable-libc++.dll> -DependencySources <source-archive-folder> -OutputDirectory build/dist
```

The runtime-root is read strictly through the committed redistributable file/hash allowlist. It is never
recursively copied. Runtime helper tools and game-library headers come from this source tree. Bridge ZIP
and dependency source archives are hash-checked. All package paths and all hashes are checked before
installation. Tests cover preserving user files, retiring known legacy executables, corruption refusal,
path escape/game-payload refusal and rollback after a copy failure.

The full archive includes corresponding project source, bridge source/notices and legal dependency
sources. It excludes all game-derived files. The GUI installer is built from tools/windows/setup_app.py
and installer_core.py with PyInstaller's windowed bootloader. It contains the same audited ZIP payload.
