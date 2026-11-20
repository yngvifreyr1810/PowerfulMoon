A fix for a bug that 0.4.4 introduced on the Steam Deck.

## Fixed

- **Steam Deck / Handheld console mode: the pause menu's background was black, its footer was missing and its layout sat too far left.** The HUD sharpening added in 0.4.4 switched on in Handheld mode, where it does not fit how the picture is drawn. Handheld is the Steam Deck's default console mode, so this affected Deck players on 0.4.4 and 0.4.5. Sharpening now stays off in Handheld mode.
  - Reproduced and checked on Windows at 1280x800 in Handheld mode (Fill and 16:9), and on a Steam Deck connected to a 1080p screen. Not yet looked at on the Deck's own screen.
  - Other screens that draw over the game's picture in Handheld mode may have been affected the same way; if something still looks wrong there, please report it with a screenshot.

## Not tested in this release

- AMD and Intel graphics cards on Windows.
- The Steam Deck beyond the pause menu check above and the release's automatic check (it starts and plays for two minutes).

## Updating

Windows: download the zip below and extract it over your current folder (or use **Updates** in the menu). Linux / Steam Deck: download the AppImage below, or use **Updates** in the menu. Your game files, saves and settings stay.
