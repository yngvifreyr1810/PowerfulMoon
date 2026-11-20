More work on the poison slowdown, shaders that were compiled late, and shorter waits after installing and updating. Numbers are from the test PC (Windows, RTX 4090, Ryzen 9950X3D) or a Steam Deck where it says so; other machines will differ.

## Performance near poison (Wooded Kingdom)

0.4.7 stopped the game fetching its poison map from the graphics card on every frame. This release goes further:

- **While Cappy clears poison** the frame rate still fell, because the map really changes then. The port now takes its copy of the map at the moment the map is drawn. Replaying a recorded session on the test PC (4K render, 120 FPS asked): 101-112 FPS while clearing before, 118-119 after.
- **Steam Deck:** the same recording replayed in Handheld mode at 60 FPS ran at 22-47 FPS on 0.4.6, and runs at 36-61 FPS (mostly 44-58) now. **It is not a steady 60 there yet** while poison is being cleared; that work continues.
- Checked again on game 1.0.0 in the Spewart fight, because this touches the code behind issue #3: Mario keeps his health on clean ground, loses a heart in visible poison, and can walk the path Cappy clears. A check that compares the poison data with and without these changes found no difference on Windows or on the Deck.
- If your graphics card usage looks high near poison after this: the game now draws about twice as many frames there. On the test PC, DLSS preset L alone took 3.7 ms of each 6.8 ms frame at 4K and 352 W against 172 W with the Default preset, at the same frame rate.

## Shaders

- **733 more shaders are compiled ahead of time.** Our test installs had recorded shaders the game draws that were missing from the shipped list, so on a fresh install they were compiled only when first needed and their draws were skipped until then. On a six-kingdom route run as a fresh install, 179 draws were skipped before and none after, with the same RAM use (about 7.7 GB at the end of the route). A new game's opening was not improved in our tests: it showed no skipped draws before or after, but those runs skip the cutscenes.
- **Shaders are no longer prepared again after every update** - only when the shader code, the graphics card or its driver changed. Because the shader list changed in this version, **this update prepares them once more**; later updates that do not touch shaders will not.

## Installing

- **The game code builds faster.** The few largest files are compiled first and with lighter settings: 167 s -> 123 s on the test PC, 1,068 s -> 942 s on a Steam Deck. Existing installs are not rebuilt by this update.
- The installer's page is drawn 60 times a second while it works (it was about 15 and looked stuck).

## Steam Deck

- About 5% less CPU use in our test scene (Cap Kingdom, 60 FPS, Handheld mode), from four small changes to how textures and shaders are looked up. Measured in that one scene only.

## Not tested in this release

- AMD and Intel graphics cards on Windows.
- The poison changes on hardware other than the test PC and one Steam Deck; finishing the Spewart fight.
- The Steam Deck's own screen: it was connected to a 1080p display for these tests.

## Updating

Windows: download the zip below and extract it over your current folder (or use **Updates** in the menu). Linux / Steam Deck: download the AppImage below, or use **Updates** in the menu. Your game files, saves and settings stay.
