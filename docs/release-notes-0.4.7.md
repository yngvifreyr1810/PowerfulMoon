A fix for a large slowdown near poison in the Wooded Kingdom, and no more needless game code rebuilds on Linux and the Steam Deck.

## Performance

- **Wooded Kingdom, near poison: the frame rate fell to about 50 FPS and stayed there**, whatever the PC. The game checks its poison map every frame, and the port fetched that map from the graphics card each time and waited for the card to finish all its queued work first. The map is now fetched only when it has changed.
  - Measured by replaying a recorded walk on the test PC (Windows, RTX 4090, game 1.4.1, 4K render, 120 FPS asked): the slow stretches ran at 50-57 FPS before and 114-119 FPS after. Other PCs and settings will show other numbers.
  - This touches the code behind issue #3 (damage from poison that isn't shown), so that was re-tested on game 1.0.0 in the Spewart fight with the issue's save: Mario keeps his health on clean ground and loses one heart in visible poison, the same as before the change. A check that compares the poison data with and without the change found no difference in about 15,000 comparisons on both game versions.
  - Not tested: clearing poison with Cappy, playing the fight to its end, AMD and Intel graphics cards, and the Steam Deck (where this code also runs).

## Linux / Steam Deck

- **Updating no longer rebuilds the game code when nothing in it changed.** Since 0.4.4, a change to a build script that only the Windows release uses made every Linux and Steam Deck install rebuild its game code after updating - about 20 minutes on a Steam Deck - for an identical result. That script now only counts on Windows, and game code built by 0.4.4 to 0.4.6 on Linux is kept. Checked on a Steam Deck: the new build started with the existing game code and was in gameplay 31 seconds later.
  - The shaders are still prepared once more after every update (several minutes on a Steam Deck). That is not changed yet.

## Not tested in this release

- AMD and Intel graphics cards on Windows.
- The Steam Deck beyond the check above and the release's automatic check (it starts and plays for two minutes).

## Updating

Windows: download the zip below and extract it over your current folder (or use **Updates** in the menu). Linux / Steam Deck: download the AppImage below, or use **Updates** in the menu. Your game files, saves and settings stay.
