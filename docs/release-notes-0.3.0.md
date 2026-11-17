A big update: no more shader stutter after updates, frame rates up to 240, a sharp HUD with every upscaler, ultrawide HUD at the edges, Switch-accurate motion controls, Brazilian Portuguese, and a faster start.

**The first start of 0.3.0 asks to compile the shaders again** (about 2 minutes on a fast PC, about 6 on a Steam Deck). Let it finish: that is what keeps the game from stuttering.

## Shaders: no more stutter after updating

- **The shader cache is re-prepared when it no longer matches.** Before, a cache that had been prepared once was trusted forever - after a GPU driver update (the driver rejects the saved shaders) or a new version of the game, every effect was built again while playing: the stutter many of you saw. The cache now remembers the GPU, driver and version it was made for, and the launcher offers to compile again when any of them changes.
- **Compiling is about 20% faster** on CPUs with many cores (it now uses every core but one; RTX 4090 / 32 threads: 137 s -> 110 s from nothing).
- Tested from an empty cache (the driver's own cache emptied too): after the compile, no shader is built while playing on PC or Steam Deck.

## Frame rate and smoothness

- **Any frame rate up to 240 FPS**: 60, 72, 90, 100, 120, 144, 165, 180 or 240 (Performance > Frame rate). Every frame is really drawn - Mario and the camera where they are at that moment, no generated in-between images. Gameplay itself (physics, enemies, timers) keeps the game's own 60 steps a second, as it was made.
- **Match display** picks the smoothest rate for your screen: the highest one up to 120 your refresh rate divides (a 144 Hz monitor: 72 FPS, every frame on screen equally long). 60 FPS on a 144 Hz screen can't be smooth - frames alternate 2 and 3 refreshes - which is the judder some of you saw even with VSync on.
- **Frame pacing on Windows**: the game's timer now follows when your screen really shows each frame (as on the Steam Deck already).
- **Faster start**: about 1.5 s less before the game appears (NVIDIA's DLSS check runs in the background).

## Upscaling and the HUD

- **A sharp HUD with DLSS, FSR and XeSS**: the HUD is now drawn at your screen's full resolution after the upscale, instead of being upscaled with the 3D scene (blocky text and icons at Performance modes, smearing with temporal upscalers). Same for lower internal resolutions.
- **AMD FSR 2** upscaler on Linux and the Steam Deck (and Windows), run by the game's own code - no extra files. Works at the Deck's defaults (100% resolution, 16:10) in Docked console mode.
- **Frame generation** (Windows): AMD FSR frame generation, and NVIDIA DLSS frame generation (beta) with **NVIDIA Reflex** low latency (Off / On / On + Boost). Off by default; needs an upscale method and 60 FPS.
- **Internal resolution** now shows the actual resolution (e.g. 2560 x 1440) instead of a cut-off "Match display".

## Ultrawide

- **HUD position (wide)**: Edges (default) puts the coin and moon counters, life gauge, button guides, mini map and "Saving..." in the corners of a 21:9 or 32:9 picture, as on a 16:9 screen; Centered keeps the HUD in a 16:9 box. Menus, the title screen and messages stay in the middle.

## Controls

- **Motion controls as on the Switch**: the game now gets its motion sensor at the Switch's 200 readings a second (it reads 16 of them every frame) instead of one per frame - quick flicks and shakes are no longer lost - and the controller's orientation is corrected by gravity, so it doesn't drift.
- **The configuration page before the game works with a controller** (D-pad / stick to move, A to change, Start to play).
- **Skip the start-up controller screen** now hides it completely: straight to the title.
- **Mouse camera** turns the camera exactly as far as the mouse moves (#52).
- **Gyro**: invert X / Y and Calibrate (#53).

## Audio

- **5.1 surround** output (Audio > Audio output) (#35).

## Languages

- **Brazilian Portuguese** (community contribution by Caio Silva, #49, #58): a Language tab in the menu; selecting Português (Brasil) can install the JumpManClub Brasil translation after asking, from its authors' own link, checked against its official files.

## Fixes

- Blocky picture when turning the camera fast with the game's dynamic resolution (#51).
- Fewer copies a frame in blur and bloom (up to ~6% faster on integrated GPUs).

## Still being worked on

- Shadows that disappear depending on where the camera points (in a few places, e.g. the Sand Kingdom ruins) - being investigated.
- FSR 2 in Handheld console mode on the Steam Deck.
- DLSS frame generation is a beta.
