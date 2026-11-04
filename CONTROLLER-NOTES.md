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
the loopback [ProCon2Bridge v1 protocol](https://github.com/yngvifreyr1810/ProCon2Bridge/blob/main/HD-RUMBLE.md).
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
* Native Windows build linked successfully. Existing 0.1.6 generated game library
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
make -j12 DEPS=/clang64 GAMELIB_ID=101fc594358477be build/PowerMoonRecomp.exe
clang tests/test_motion.c -lm -o build/test_motion.exe
clang tests/test_hd_wire.c -o build/test_hd_wire.exe
build/test_motion.exe
build/test_hd_wire.exe
```

The explicit ID targets the upstream 0.1.6 Windows game-code ABI. Do not change
generated code or the SDK symbol inventory for this update. No game data is
needed to build the host or these tests. Release compiler: MSYS2 CLANG64; SDL3
headers 3.4.18, FFmpeg headers 9.0.2, Vulkan 1.4.363, shaderc 2026.4, VMA 3.5.
Release includes libc++.dll; its source and license are linked in CREDITS.md.
