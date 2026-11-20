## Fixed / new

- **Steam Deck motion controls with Steam Input on** (#68, #89, #100): the gyro is read from the Deck's own controller, so motion throws work without turning Steam Input off.
- **Steam Deck in the Wooded Kingdom's poison**: about 59 FPS on average in our recorded run (was about 52).
- **Less memory on Windows** (#99, #95, #87): committed memory is about half what it was (13 GB → 6–7 GB in our tests). 8 GB PCs should get much further.
- **Controls**: deadzone and range sliders for each stick (default 5%), gyro sensitivity, and a shake threshold so rumble doesn't throw Cappy (#106, #107). Pressing a key while a controller cell waits now sets the keyboard binding (#108).
- **Graphics**: Bloom quality (High smooths the glow and the far horizon, #102) and Draw distance (High 2x, Very high 4x).
- **Start page**: a "Compile shaders now" button when shaders need compiling (#101).
- **Crash reports** start with the reason, and Copy puts the report's text on the clipboard (#75).
- The menu pointer hides while you use a controller or the keys (#105, thanks @galaxyhf).
- Korean and Portuguese for the new options.

## Still to fix

- Gyro aiming through binoculars turns left/right the wrong way (#53).
- Intel integrated graphics may still run out of graphics memory (#99).
- Not reproduced yet: the Steam Deck camera spinning with Steam Input off (#100), black dust squares on some AMD cards (#103).

## Plan

Gyro aiming next, then the Intel memory limit.

## Updating

This update rebuilds the game code once (a few minutes on a PC, about 15 minutes on a Steam Deck) and prepares the shaders once. A stick deadzone left at 0% or 10% becomes the new 5% default.

Windows: download the zip below and extract it over your current folder (or use **Updates** in the menu). Linux / Steam Deck: download the AppImage below, or use **Updates** in the menu. Your game files, saves and settings stay.
