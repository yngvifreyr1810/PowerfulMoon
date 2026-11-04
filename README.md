# PowerfulMoon

**A customized fork of [Power Moon Recomp by zzzarathustra-dev](https://gitlab.com/zzzarathustra-dev/power-moon-recomp), based on upstream v0.2.0.**
Maintained by **Vĩnh An Project / yngvifreyr1810**, with implementation assistance from OpenAI Codex.

PowerfulMoon combines upstream's native Odyssey runtime and 120 fps frame interpolation
with the tested VRR fix, Switch 2 Pro Controller motion/rumble, and persistent controller preferences.
The upstream-authored crescent icon is retained. No Cappy or other Nintendo artwork is included.
This project is independent of Nintendo and the upstream maintainer.

## Install or upgrade

Download from [Releases](https://github.com/yngvifreyr1810/PowerfulMoon/releases):

- **PowerfulMoon-Setup-0.2.0.exe**: full Windows installer, recommended.
- **PowerfulMoon-0.2.0-windows-x64.zip**: the same full app as a portable folder.

No Python or separate compiler installation is required. This is a complete app distribution,
not the old two-file controller patch. It includes the host, SDL/FFmpeg runtime, local game-code
setup tools, separate ProCon2Bridge 0.4.0, corresponding project source and dependency notices/source archives.
You must supply your own Super Mario Odyssey 1.0.0 game files; none are downloaded or bundled.

**New installation:** run Setup, choose a writable folder and open **PowerfulMoon.exe**.
Its setup page imports your own extracted game files and builds the game-code library locally.
The bundled controller bridge starts when needed; its signed USB/IP driver may ask for Windows
administrator approval once. The bridge subsequently runs in the notification area and can
start with Windows; the game itself is never registered for Windows startup.

**Upgrade from Power Moon 0.1.6 / 0.2.0 or an earlier controller build:** save and close the game,
run Setup and choose the **existing Power Moon game folder**. Save files, game files, mods,
video settings, calibration and controller profiles stay in place. Replaced program files and
copies of settings are backed up under `powerfulmoon-backups/`. Known old EXEs/launchers are
retired into that backup; unknown or user-modified files are left alone. Use **PowerfulMoon.exe**
afterward. The game-code cache rebuilds once because its imports now target the renamed host.
The original folder may keep its old name; renaming or moving a multi-GB game folder is unnecessary.

A Start-menu shortcut is created by Setup. For a portable install, extract the ZIP and open
PowerfulMoon.exe; extract over the same folder for later PowerfulMoon updates, keeping your user files.
Use matching PowerfulMoon releases for updates; copying a different upstream runtime over the
customized app is not a supported PowerfulMoon upgrade path.

## Switch 2 Pro Controller

Press a controller button to reconnect; hold SYNC for first pairing. In **F1 > Controls**,
choose motion, vibration, direct haptics and strength; **Test vibration (2 seconds)** gives a
sustained test. GL/GR mappings and gyro calibration are managed by the bridge's tray menu.
Closing a CMD window is not required; the bridge has no console window.

The host keeps **powermoon.ini** and **powermoon.ini.controller.ini** for migration compatibility.
The latter preserves motion, rumble strength and full button bindings independently of upstream's
settings writer. Earlier settings already erased before a profile/backup existed cannot be
reconstructed; missing preferences use defaults. ProCon2Bridge's settings stay in its own user app folder.

Motion and rumble were confirmed by the user in the 0.2.0 integration. The bridge has also been
reported working with emulators, but this host's gameplay verification is specifically Odyssey.
Direct per-side, per-band haptics are experimental; console-identical HD Rumble / HD Rumble 2 is not established.

## VRR and 120 fps

In **F1 > Video**, enable Frame interpolation. **Interpolation output: 120 fps (VRR)** keeps
interpolation enabled when a high-refresh display is temporarily reported at 60 Hz. A supported
~120 Hz or faster mode at the current resolution is required. The user confirmed VRR enabled,
~120 fps and working motion/rumble. The game logic remains at 60 Hz; GPU load and upstream's
safety brake can still reduce the actual presentation rate.

**VSync: Auto / On / Off** selects host presentation (mailbox / FIFO / immediate). Restart the
game after changing it. Off may tear outside the VRR range; Windows/driver VRR settings are not changed.
Corresponding settings:

```ini
frame_interp = 1
interpolation_output = 120
vsync = auto
```

## Source and credits

Host and customizations: **MIT**, with original notices retained. The bundled **ProCon2Bridge**
is a separate **GPL-3.0-or-later** executable; it is not linked into the MIT host. Other components
retain their own licenses. See [CREDITS](CREDITS.md), [upstream documentation](UPSTREAM.md),
[controller/build notes](CONTROLLER-NOTES.md) and [security/removal](SECURITY.md).

The portable package includes `source/` for this project, `controller-bridge/` for the bridge's
source/notices and `licenses/` for dependencies. Upstream authored the base runtime, renderer,
frame interpolation, setup flow and moon icon. This fork adds controller support/persistence,
VRR output selection, VSync settings, rebranding and the integrated full installer.

No ROM, keys, firmware, Nintendo SDK/font/artwork, game screenshot, extracted game asset, save,
or generated game-code library is distributed. **odyssey_game.dll must never be uploaded.**
Only original open-source host/tools and redistributable dependencies are included.
