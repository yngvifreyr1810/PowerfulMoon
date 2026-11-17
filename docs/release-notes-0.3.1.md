A fix update: shadows no longer disappear when the camera moves, CPUs without AVX2 are supported, and the settings menu and narrow screen shapes are tidied up.

## Shadows

- **Shadows stay where they belong.** Big shadows - the Odyssey's over Mushroom Kingdom's plaza, fountains, trees, buildings - vanished or popped in and out when the camera looked down or Mario took a few steps, all over the game. The game asks the GPU to keep everything between the sun and its shadow map ("depth clamp"); that request was being ignored, so whatever came close to the camera's view was cut out of the shadow map. It is now applied. Checked side by side against the original game's rendering: level, looking up and looking down give the same shadows. No change in frame rate.

## Older CPUs (#67)

- **CPUs without AVX2 can now run the game** - Intel Sandy Bridge / Ivy Bridge (2011-2012), and Pentium / Celeron models without AVX2. The game code is compiled on your PC for the CPU it has; before, anything older than Intel Haswell / AMD Zen was refused at start.
- **Nothing changes on a CPU with AVX2**: same settings, and the game code you already compiled stays as it is (no rebuild).
- On an older CPU, the first start compiles the game code for it (a few minutes). Expect the frame rate those older CPUs allow - the game is demanding on the CPU. **Tested only by simulating an older CPU on a new one (Zen 5)**; reports from real older CPUs are very welcome.

## Settings menu

- Each tab now shows its own descriptions (the Display tab's Monitor text was repeated on Graphics, Audio, Mods and Updates).
- All tab names fit ("Performance" and "Language" were cut short), and only the open tab is highlighted.
- The menu window is nearly opaque, so the game's own menu no longer shows through behind the settings.
- The Internal resolution description is complete again.

## Screen shapes

- **16:10 and 4:3 pause menu**: the bottom bar lines up with its "Select / Confirm" text again.
- Cutscenes checked at 16:9, 16:10, 21:9 and 32:9: clean black bars.

## Updating

Windows: download the zip below and extract it over your current folder (or use **Updates > Check for updates** in the menu). Linux / Steam Deck: download the AppImage below. Your game files, saves and settings stay.
