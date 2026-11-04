# PowerfulMoon 0.2.0 validation

- The user confirmed the 0.2.0 controller integration and the VRR fix: about 120 fps with VRR enabled, with motion and rumble still working.
- Runtime logs in that test reported about 60 game frames/s plus about 60 in-betweens/s. Game simulation timing remains unchanged.
- Motion math / sensor ABI, detailed haptic wire, refresh-rate selection, and controller-profile migration/recovery checks passed locally.
- Full installer tests passed: preserving settings/save/game sentinels, exact legacy-host retirement, repeated install, payload checksum rejection, path/game-content refusal and rollback after a copy failure.
- An actual full-package migration kept the existing main settings, controller sidecar and two save files byte-for-byte unchanged at install time. The recognized old EXE was backed up and the renamed PowerfulMoon.exe opened the local cache rebuild flow.
- A clean package contains only audited, allowlisted runtime/tool/source/license files. No game library, extracted assets, saves, keys or Nintendo artwork are distributed. The source archive is taken from tracked project files, and runtime redistributables are taken from an explicit SHA-256 allowlist.
- The upstream-authored crescent icon remains unchanged; the proposed Cappy image was excluded at the user's request.
- Bridge Bluetooth/startup verification was completed previously in ProCon2Bridge 0.4.0, including a successful user-confirmed Windows restart test. The bridge is bundled unchanged and remains separately licensed.
