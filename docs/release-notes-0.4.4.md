Smaller release: less stutter in one heavy place, two picture-shape fixes, sharper HUD on large screens, and a change that may help CPUs without AVX. Everything below was measured on one Windows PC (RTX 4090) unless it says otherwise; results on other PCs will differ.

## Performance

- **New Donk City, riding the power lines:** two costs in the port's own code grew with every command list the game records, and this view records a lot of them. On a recorded ride replayed at 60 FPS on the test PC, frames longer than 17 ms went from 73 to between 7 and 18 per ride. It is not gone: at 120 FPS that ride still drops frames, and in 2 of 7 replays there was one long frame (75 ms, 631 ms) we could not explain.
- **Frame rate option:** a note that above 60 FPS the busiest places can drop frames on any PC.

## Picture

- **Ultrawide (#93):** a chosen screen shape within 3% of the window's own shape now fills the window. "21:9" on a 3840x1600 screen left thin bars at the sides and rendered at a slightly lower resolution. Checked by resolution in a window here, not on a 3840x1600 monitor.
- **16:10 screens (Steam Deck):** the dark bands behind button hints and tutorial text stopped short of the screen's bottom or top edge and left a gap; they now reach the edge. Checked at 1280x800 on the PC, not yet on a Deck itself.
- **Sharper HUD and menus (new option, Graphics, on by default):** the game's HUD, text and menus are drawn from artwork made for 720 lines and looked soft when enlarged. On screens of 900 lines and more they are now sharpened, more on bigger screens. It makes edges crisper; it does not add detail. Looked at on 1440p and 4K output sizes with upscaling off; not checked with every upscaler or on ultrawide. Below 900 lines (a Steam Deck's own screen) nothing changes. If it looks too hard to you, turn it off.

## Menu

- The FPS counter and the performance overlay options moved from **Performance** to their own page, **Overlay**.

## CPUs without AVX (#87)

- Thanks to **galaxyhf** (pull request #92): the runtime that is linked with the game code was built for CPUs with AVX2, so on a CPU without it the game stopped at Play with an illegal instruction. It is now built for the baseline x86-64. The built runtime was checked to contain no AVX instructions, but **it was not tested on a CPU without AVX**. If you have one, please tell us in #87 whether it now starts.

## Other

- The log no longer prints a frame generation (DLSS-G) status line while frame generation is off; it read as if it were on.

## Not tested in this release

- AMD and Intel graphics cards.
- The Steam Deck beyond the release's automatic check (it starts and plays for two minutes).

## Updating

Windows: download the zip below and extract it over your current folder (or use **Updates** in the menu). Linux / Steam Deck: download the AppImage below. Your game files, saves and settings stay. On Windows the game code is rebuilt once at the first start after this update, which takes a few minutes.
