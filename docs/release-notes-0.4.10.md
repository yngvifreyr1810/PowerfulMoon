A faster Steam Deck near poison, and three small things from the issues. Numbers are from one Steam Deck (LCD, Handheld mode, 60 FPS asked) or the test PC (Windows, RTX 4090) where it says so; other machines will differ.

## Steam Deck: the Wooded Kingdom's poison

0.4.8 left the Deck at 36-61 FPS while poison is being cleared. The game asks for its poison map several times a frame, and the port answered only between two batches of drawing - one batch is most of a frame, so the game waited. It now answers as soon as nothing left in the batch draws into the map.

- The same recorded session replayed on the Deck, in two-second windows: **36-61 FPS (average 51.6) on 0.4.9, 45-61 FPS (average 59) now**; 18 of 38 windows were under 50 FPS before, 1 to 3 now. Started through Steam in Gaming Mode: average 59.1.
- **It is still not a steady 60 there.** One stretch near the start of that area runs at 45-50 FPS for a few seconds.
- Windows was already at its frame cap in this recording and stays there.
- Checked on game 1.0.0 in the Spewart fight (the code behind issue #3), on Windows and on the Deck: Mario keeps his health on clean ground, loses a heart in visible poison, and can walk the path Cappy clears. Two checks that compare every early answer with the old way found no difference in more than 500,000 answers across both machines and both game versions. Both versions were also played by hand on the Deck and on Windows.

## From the issues

- **Compile the shaders without starting the game (#101):** the shader question on the start page has a new choice, **Compile, play later**. It compiles and returns to the start page. (English only for now.)
- **"Create shortcut" on the Steam Deck refusing with "No unambiguous Steam account" (#81):** an empty folder that Steam keeps for "no account" was counted as a second account. It is ignored now. Not confirmed on the reporter's Deck yet.
- **Running out of memory on PCs with 8 GB and integrated graphics (#99):** the game now says that memory ran out and what to try, instead of a bare error. It does not use less memory than before.

## Not tested in this release

- AMD and Intel graphics cards on Windows.
- The poison change on hardware other than one Steam Deck and the test PC.

## Updating

Windows: download the zip below and extract it over your current folder (or use **Updates** in the menu). Linux / Steam Deck: download the AppImage below, or use **Updates** in the menu. Your game files, saves and settings stay. This update does not rebuild the game code or prepare the shaders again.
