Sorry, I did this since I found the crackling noise and it's been fixed now.

## Sound

- **Crackling near characters (#97):** every new sound played its first 5 ms at the volume left behind by the last sound in the same slot, then slid to its own volume. A quiet, distant sound (a character's footstep) could start at a loud nearby sound's volume, which was heard as a tick at the start of each one. A new sound now starts at its own volume, as on the console.
  - Reproduced with a recorded walk through New Donk City, the Odyssey and the Cap Kingdom on the test PC (Windows, RTX 4090): about 100 seconds had 425 sounds starting at a wrong volume and 138 isolated ticks in the mixed sound before, none after. Confirmed by ear on that PC, in stereo and 5.1.
  - Not listened to on a Steam Deck or on Linux. Other kinds of crackling (the sound device running dry on a busy PC) are a different cause and are not changed by this. If you still hear it, please attach `SuperOdysseyRecomp.log` to #97.

## Menu

- The hints at the bottom of the menu are now icons for what you are using: key caps and arrows for the keyboard, and drawn buttons for a Switch, Xbox or PlayStation controller. They switch when you switch device. Checked in screenshots for each of the four layouts; which controller is connected is detected by the code the menu already used, and was not re-tested with real PlayStation or Switch controllers.

## Not tested in this release

- AMD and Intel graphics cards.
- The Steam Deck beyond the release's automatic check (it starts and plays for two minutes).

## Updating

Windows: download the zip below and extract it over your current folder (or use **Updates** in the menu). Linux / Steam Deck: download the AppImage below. Your game files, saves and settings stay.
