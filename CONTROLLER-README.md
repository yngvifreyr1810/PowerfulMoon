# PowerMoon-Switch2Pro

An independently maintained **MIT** fork of Power Moon Recomp **v0.1.6**, adding
controller motion and vibration to the native Odyssey runtime. This is a controller
compatibility update, not a game download. Maintainer: **Vĩnh An Project / yngvifreyr1810**;
implementation assistance: OpenAI Codex.

**Gameplay testing of this controller solution has been limited to TOTK Recomp
and Super Mario Odyssey / Power Moon Recomp. No emulator (including Ryujinx or
Eglantine) has been tested.**

Before running, read [script behavior, permissions and integrity checks](SECURITY.md).
The project executable is unsigned; antivirus protection should remain enabled.

## Easy Windows update

1. Set up [ProCon2Bridge](https://github.com/yngvifreyr1810/ProCon2Bridge/releases) once:
   extract its Windows ZIP, double-click START.cmd, then hold SYNC on the controller.
2. Download **PowerMoon-Switch2Pro-0.1.6-s2pc.1-windows-x64-update.zip** from
   [Releases](https://github.com/yngvifreyr1810/PowerMoon-Switch2Pro/releases), and extract.
3. Save and close your existing **Power Moon Recomp 0.1.6**. Double-click
   **INSTALL.cmd**, select its PowerMoonRecomp.exe. The installer verifies the supported
   runtime, backs up the two affected program files and installs the update.
4. Use **PLAY.cmd** in the update folder thereafter. It remembers your selected game
   folder, asks for the bridge folder once if needed, starts the bridge and launches
   the game. No Python, compiler or manual settings editing is needed.

Windows 11 x64 was tested. This update requires an existing compatible upstream
0.1.6 installation; it does not install game data or replace a complete upstream
distribution. It refuses other executable versions. [Upstream setup/source](https://gitlab.com/zzzarathustra-dev/power-moon-recomp).

In **F1 > Controls**, motion and vibration default on. The experimental direct
haptics option retains each side's low/high frequency and amplitude. Vibration
strength is adjustable from 0x to 2x. **Test vibration (2 seconds)** maintains a
two-second signal even while gameplay submits other effects.

Motion and ordinary event timing were tested in gameplay. The user confirmed the
two-second test and a modest subjective improvement with direct haptics. Physical
frequency calibration remains experimental: **this is not verified console-identical
HD Rumble / HD Rumble 2**. See [CONTROLLER-NOTES.md](CONTROLLER-NOTES.md).

## Restore

Close the game, run **RESTORE.cmd** from this update folder and choose the same
PowerMoonRecomp.exe. It restores the most recent complete backup made by INSTALL.cmd.
Your save, settings, cache and own game files are not part of this update.

## Source, licenses and content policy

The repository is a source snapshot based on upstream tag v0.1.6, commit
`2957896536727cdff26598bdd007733e65c6af89`. The original copyright notices,
MIT license and third-party notices are retained. Read [CREDITS.md](CREDITS.md)
and [UPSTREAM.md](UPSTREAM.md). Build instructions are in CONTROLLER-NOTES.md.
The separate GPL bridge communicates via the documented local protocol; its
implementation is neither linked nor copied into this MIT host.

No ROM, keys, console firmware, Nintendo SDK, font, game screenshot, extracted game
asset, save, or generated game-code library is included. `odyssey_game.dll` and
anything built from the user's game data must never be uploaded. Only original
open-source host code and redistributable dependency notices are published here.
The update binary links to the user's existing SDL/FFmpeg runtime and bundles
only the additional LLVM libc++ runtime with its license. Nintendo's trademarks
remain Nintendo's; this project has no affiliation or endorsement.
