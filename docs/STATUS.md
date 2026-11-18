# Status

[Back to the README](../README.md)

Where each part of Super Odyssey PC Recomp stands, as of version 0.4.2. The numbers are the maintainer's estimates,
not measurements: they say how finished a part is, with the open issues that hold it back.

The overall bar in the README is a weighted average. Each area has its share of the whole (the percentage in its
heading). Each part has a weight inside its area: x1 an extra, x2 important, x3 essential. An area's value is the
weighted mean of its parts, and the overall value the weighted mean of the areas. After changing a table, run
`python tools/status_bars.py` to redraw the bars.

✅ done · 🔧 in progress · 🗺️ roadmap

### Core game (45%)

| | Part | Progress | Weight | What it means |
|---|---|---|---|---|
| ✅ | **Recompiler** | ![100%](media/progress/100.svg) | x3 | The whole game (main, sdk and subsdk modules) is translated to native code on your PC |
| 🔧 | **Horizon OS services** | ![95%](media/progress/95.svg) | x3 | Threads, files, saves, controllers, applets and time: everything the game uses. Online services aren't provided |
| 🔧 | **Maxwell shader translator** | ![95%](media/progress/95.svg) | x3 | Every shader of the game's archives (about 32,800 programs) translates to Vulkan |
| 🔧 | **Graphics (NVN on Vulkan)** | ![90%](media/progress/90.svg) | x3 | Every kingdom draws on NVIDIA, AMD, Intel and the Steam Deck. Open: a GPU hang on Radeon RX 7800 XT under Linux ([#21](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/21)), a tutorial video and blocky shadows on an RX 6700 XT ([#66](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/66)) |
| 🔧 | **Tested in game** | ![80%](media/progress/80.svg) | x3 | Every kingdom is visited by scripted tours on both game versions, with paintings, the wardrobe and the map. Open crash reports waiting for their reporters: [#69](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/69), [#55](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/55), [#79](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/79) |
| 🔧 | **Sound** | ![95%](media/progress/95.svg) | x2 | The game SDK's audio renderer with reverb and 5.1 surround. The crackling during loads on Windows ([#83](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/83), [#84](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/84)) is fixed in 0.4.2 |
| ✅ | **Game files setup** | ![100%](media/progress/100.svg) | x2 | The start screen copies your extracted game, builds the game code and prepares the shaders |
| 🔧 | **Game versions 1.0.0 and 1.4.1** | ![90%](media/progress/90.svg) | x1 | Both play. 1.4.1 ships without function names, so a few test tools and fixes reach it later |

### Platforms and input (15%)

| | Part | Progress | Weight | What it means |
|---|---|---|---|---|
| 🔧 | **Windows** | ![95%](media/progress/95.svg) | x3 | The main platform: every release is played on it before it goes out |
| 🔧 | **Steam Deck** | ![80%](media/progress/80.svg) | x3 | 1280 x 800 at 60 FPS in Gaming Mode. Open: the Deck's own gyro ([#68](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/68)), a no-sound report ([#43](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/43)) |
| 🔧 | **Linux** | ![75%](media/progress/75.svg) | x2 | An AppImage, checked by the AppImage catalog's own test before every release. Less tested than Windows |
| 🔧 | **Controllers** | ![90%](media/progress/90.svg) | x2 | Xbox, PlayStation, Switch Pro and a Joy-Con pair, with matching button icons and rebinding |
| 🔧 | **Keyboard and mouse** | ![90%](media/progress/90.svg) | x2 | Rebindable keys, a real mouse camera, and the mouse in the game's own menus ([#64](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/64)) |
| 🔧 | **Motion controls** | ![70%](media/progress/70.svg) | x1 | PlayStation and Switch Pro controllers. Not yet the Steam Deck's built-in gyro |

### Performance (15%)

| | Part | Progress | Weight | What it means |
|---|---|---|---|---|
| 🔧 | **No shader stutter** | ![90%](media/progress/90.svg) | x3 | Every shader is prepared before play and again after a driver update. A rare effect nobody has recorded can still hitch once |
| 🔧 | **Frame pacing** | ![90%](media/progress/90.svg) | x2 | The game's timer is locked to when the screen shows each frame |
| 🔧 | **Frame rates above 60** | ![85%](media/progress/85.svg) | x2 | 72 to 240 FPS with every frame really drawn. Scenes not prepared for it (painting transitions) play at 60 |
| 🔧 | **Memory** | ![90%](media/progress/90.svg) | x2 | Steady over long sessions, and since 0.4.3 about 6 GB in RAM on NVIDIA (was 12-15 GB: [#94](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/94), [#75](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/75), [#61](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/61)). Not yet measured on AMD and Intel |
| 🔧 | **Low-end PCs and handhelds** | ![60%](media/progress/60.svg) | x2 | Handheld mode and lower resolutions help; a tested floor (4 cores, integrated graphics, 8 GB) is still to do |

### Graphics options (12%)

| | Part | Progress | Weight | What it means |
|---|---|---|---|---|
| ✅ | **Resolution and screen shape** | ![100%](media/progress/100.svg) | x3 | Up to 4x internal resolution, 16:9, 16:10, 21:9, 32:9, 4:3 or Fill, the HUD at the edges |
| 🔧 | **Upscalers** | ![90%](media/progress/90.svg) | x3 | NVIDIA DLSS / DLAA, AMD FSR 1 / 2 / 3.1 / 4 (FSR 4 on Radeon RX 9000 only), Intel XeSS, with the HUD drawn at full resolution |
| 🔧 | **Frame generation** | ![80%](media/progress/80.svg) | x2 | AMD FSR (Windows) and NVIDIA DLSS (beta) |
| ✅ | **Anti-aliasing, shadows, textures** | ![100%](media/progress/100.svg) | x2 | FXAA, SMAA, 2x / 4x shadow maps, texture detail, anisotropic filtering, smooth gradients |

### Player features (13%)

| | Part | Progress | Weight | What it means |
|---|---|---|---|---|
| 🔧 | **Start screen and settings menu** | ![95%](media/progress/95.svg) | x3 | Reworked in 0.4.1: Play first, categories in a sidebar, every option explained with a recommendation |
| ✅ | **Built-in updates** | ![100%](media/progress/100.svg) | x2 | Checks this page and installs the new version from the menu |
| 🔧 | **Crash reports and logs** | ![95%](media/progress/95.svg) | x2 | A report with a plain-words summary and a signature to match crashes between players |
| 🔧 | **Mods** | ![80%](media/progress/80.svg) | x2 | RomFS mods, switched on and off in the menu. Code mods (exefs patches) can't work: the code is rebuilt from the original |
| 🔧 | **Languages** | ![85%](media/progress/85.svg) | x1 | The game's own languages; the port's menus in English, Korean and Brazilian Portuguese |
| ✅ | **Discord activity** | ![100%](media/progress/100.svg) | x1 | The kingdom you're in on your profile (off by default) |
| 🗺️ | **amiibo** | ![0%](media/progress/0.svg) | x1 | Roadmap ([#48](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/48)) |
