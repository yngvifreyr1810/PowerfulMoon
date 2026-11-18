Much less memory, a faster first start and a smaller shader cache, plus a fix for a rare crash at start-up.

## Memory (#94, #75, #61, #83)

The game used 12 to 15 GB of RAM on many PCs and pushed 16 GB PCs into the page file. Measured on an RTX 4090 through six kingdoms at 120 FPS, the same route before and after: **14.7 GB at the start and 15.6 GB at the end before, 5.8 GB and 6.6 GB now**, with the same frame rate and no more stutter.

- **NVIDIA:** shader libraries are no longer created from saved pipeline binaries. NVIDIA's driver held far more memory for those than for the same libraries from a normal pipeline cache. (AMD and Intel are unchanged: on AMD it made no difference.)
- **Warm-up at start, Auto:** now prepares up to 1 GB of effects on every PC. Before, PCs with 32 GB or more prepared up to 12 GB, which bought almost nothing in our tests. Normal and Long are still in the menu.
- Only the shaders a scene needs in its first frames are built in full when the game sets them up; the others when they are first drawn.

## Shaders: faster to prepare, much smaller on disk

- The preparation before the first start now translates and compiles only the shaders the game actually draws, the effects and the compute shaders, instead of all ~32,800 in the game's files. A first start on an RTX 4090: **98 s before, 44 s now**; on a Steam Deck about **6 minutes before, a little over 3 now**. The rest are prepared if the game ever uses them, in the background.
- The shader cache is one compressed file (`cache/shaders.pack`) instead of tens of thousands of files. A first start's cache: **2.2 GB before, under 0.7 GB now** (a Steam Deck's: 254 MB). An existing cache moves into it by itself in the background while you play.
- After this update the shaders are prepared once more, as after every update (about 30 s on a fast PC).

Thanks to [Odyssey Recomp](https://github.com/rwwo8/OdysseyRecomp) (MIT license): the list of shaders drawn in a run through all 197 stages and the idea of compiling only those and packing the cache come from it. Thanks to Eggtooth for the side-by-side measurements in #94 that pointed to it.

## Fixes

- **A crash at start-up, about one start in a hundred** (`nn::hid::GetTouchScreenState` in the crash report): the game could place the controllers' shared memory on top of memory the port uses for itself. That memory is now marked as taken.

## Updating

Windows: download the zip below and extract it over your current folder (or use **Updates** in the menu). Linux / Steam Deck: download the AppImage below. Your game files, saves and settings stay.
