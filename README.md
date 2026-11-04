# PowerfulMoon

PowerfulMoon is an independent customized fork of [Power Moon Recomp](https://gitlab.com/zzzarathustra-dev/power-moon-recomp) (v0.2.0) by zzzarathustra-dev. It is maintained by **Vĩnh An Project / yngvifreyr1810** with implementation assistance from OpenAI Codex.

Upstream provides the native game runtime, renderer, frame interpolation engine, and ultrawide support. PowerfulMoon adds:

- Switch 2 Pro Controller motion controls, vibration, and persistent controller profiles.
- A VRR output selection fix and host VSync controls.
- An integrated full Windows installer and standalone app distribution.

---

## Downloads & Installation

You must supply extracted files from your own legally obtained copy of **Super Mario Odyssey 1.0.0**. No game files, keys, ROMs, or Nintendo assets are provided.

Download the latest release from [Releases](https://github.com/yngvifreyr1810/PowerfulMoon/releases/latest):

| Package | Recommended For | Details |
| :--- | :--- | :--- |
| Installer (EXE) | Most users | Guided setup, Start menu integration, and upgrade backups. |
| Portable ZIP | Manual installation | Extract to a writable folder and run `PowerfulMoon.exe`. |

### Quick Start

1. Run **Setup** or extract the portable ZIP to a writable directory on Windows 11.
2. Launch `PowerfulMoon.exe`.
3. Follow the setup page to import your extracted Super Mario Odyssey 1.0.0 game files. First-time game-code compilation takes a few minutes.

> The bundled controller bridge starts when needed. Its first setup may request permission to install the signed USB/IP driver and register the bridge for Windows startup. The game itself does not start automatically.

---

## Upgrading

To update an existing Power Moon or PowerfulMoon installation:

1. Save and close the game.
2. Run the latest Setup and select your **existing game directory**. For a manual update, back up that folder before extracting the portable ZIP into it.
3. Launch `PowerfulMoon.exe`.

Selecting your existing folder preserves game saves, mods, user files, and controller settings (`powermoon.ini` / `powermoon.ini.controller.ini`). **Setup** backs up replaced program files and settings snapshots to `powerfulmoon-backups/`; manual ZIP extraction does not. Migrating to the renamed EXE triggers a one-time game-code cache rebuild.

---

## Configuration

### Video Settings (`F1 > Video`)

- **Frame interpolation**: Generates intermediate frames. Game logic remains at 60 Hz while presentation renders up to 120 fps.
- **Interpolation output**: Select `120 fps (VRR)` if your high-refresh display temporarily reports at 60 Hz. Requires a supported ~120 Hz or faster display mode; actual output depends on performance.
- **VSync**: Choose `Auto`, `On`, or `Off`. Game restart required after modifying VSync.

### Controller Options (`F1 > Controls`)

- Reconnect controllers by pressing any button; hold SYNC for initial pairing.
- Configure game bindings, motion controls, and vibration strength. Use **Test vibration (2 seconds)** to test feedback.
- GL/GR back buttons and gyro calibration are managed through ProCon2Bridge's tray menu. Direct per-band haptics remain experimental; console-identical HD Rumble 2 is not verified.

---

## License & Credits

- **Host & Customizations**: Distributed under the [MIT License](LICENSE).
- **ProCon2Bridge 0.4.0**: Included as a separate process under [GPL-3.0-or-later](https://github.com/yngvifreyr1810/ProCon2Bridge/blob/main/LICENSE.md) (communicates via local sockets; not linked into the MIT host).
- **Upstream Engine**: Built on Power Moon Recomp by zzzarathustra-dev and contributors.

See [CREDITS.md](CREDITS.md) for full attribution, [build notes](CONTROLLER-NOTES.md), and [SECURITY.md](SECURITY.md) for permissions and verification details.

## Updates

Use **F1 > Controls > Check for updates** for approved PowerfulMoon releases. Save and close the game before installing into its existing folder. The independent upstream bot prepares candidates on GitHub; only owner-approved releases reach the app. See [Updates and approval](docs/UPDATES.md) or the [Vietnamese step-by-step guide](docs/UPDATES-VI.md).
