# Engineering notes: crashes, testing and the fixes that mattered

[Back to the README](../README.md) · Performance techniques: [OPTIMIZATIONS.md](OPTIMIZATIONS.md) ·
The current plan: [stability-plan.md](stability-plan.md)

What went wrong in Super Odyssey PC Recomp, how each problem was found, and what fixed it - written for anyone
building a recompiled or emulated port. Every number here was measured on the maintainer's PC (Ryzen 9 9950X3D,
RTX 4090) or Steam Deck OLED unless it says otherwise. The code is MIT licensed: take what helps.

## How we test

**The loop every change goes through**

1. **Measure first.** No fix without a number or a crash signature from a log or a report.
2. **Reproduce it by script.** The game runs unattended from environment variables: scripted controller input
   (`ODYSSEY_INPUT="1500:A:8,..."`: frame, button, frames held), real key and controller events for the menus
   (`ODYSSEY_HOST_INPUT="440:sdlkey:Down:2,500:shot:name"`), warps through the game's own stage-change functions
   (`ODYSSEY_WARP=world:LakeWorldHomeStage@1500`, or `hole:Go@4000` for a painting), screenshots at fixed frames, and a
   quit at a frame. A crash a player hit once becomes a run that crashes every time.
3. **One change, one switch.** Each fix sits behind an `ODYSSEY_*` switch, so the same build runs the old and the new
   behaviour back to back, alternating, on the same save and route.
4. **Gates.** Crashes, average FPS, worst frame, hitch lines, held frames, memory at the end of a soak, skipped draws,
   screenshots at fixed frames.
5. **Then a person plays it**, with a short card of exact steps.

**Rules that saved us**

- Tests run on copies of the game data and saves, never the install someone plays.
- A release is built by CI from a clean checkout, and those exact files are tested before they are published: the
  Windows build plays into the first kingdom and takes a screenshot, the AppImage starts on a Steam Deck with and
  without a Vulkan driver, and the AppImage catalog's own test runs against it. Two earlier releases shipped a Linux
  bug that only that catalog test caught, which is why it now runs first.
- Every kingdom is visited by a scripted tour (`tools/kingdom_tour.py`, [KINGDOM-TOUR.md](KINGDOM-TOUR.md)). An exit
  code alone doesn't count: the log must show the warp accepted and the stage reached.

**Ways our own tests lied to us**

| What happened | What it cost | The fix |
|---|---|---|
| The monitors were powered off for an overnight run | The graphics driver reported no active display and every present waited ~250 ms: the game ran at 4-10 FPS, so the high-frame-rate crash under test could not happen. A night of runs proved nothing | Keep the display on (cover it with a black window instead) and check for a 60 FPS title screen before trusting a run |
| A bench passed `WARP=none` to mean "no warp" | The test code took `none` as a stage name; the game crashed looking for `noneMap.byml` right after Continue. It looked like a real crash on the Steam Deck | The warp code accepts `none`; reproduce on a second machine before believing a crash |
| The test folder missed a DLL the release ships | Frame generation silently stayed off, so a reported crash "didn't reproduce" | Test the release's own files |
| A screenshot tool captured the image before the upscaler | An upscaler bug could not show in the pictures | Capture what reaches the window |
| A first sound-gap counter counted an empty queue before each block | It reported gaps that weren't audible and hid the real pattern | Count what the sound device itself reports (its short pulls) |
| A checkout with Windows line endings built the Linux package | A carriage return in the AppImage's file name, in its `.desktop` file and in `AppRun` ("execv error") | Convert line endings when sending sources to Linux; strip `\r` where a version string is read |

## Crashes

### Frame rates above 60: the game's temporary textures ([#72](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/72), [#85](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/85))

Frame rates above 60 work by drawing the game's scene again between two gameplay ticks. The game's
`agl::utl::DynamicTextureAllocator` hands out temporary textures for a frame and expects to be reset once a tick; a
second draw without that reset runs it dry, and the depth shadow map's allocation faults at address `0x20`.

- **First fix:** reset the allocator before every extra draw, whatever scene draws (the title screen's scene had been
  missed and crashed at 180 FPS).
- **Still crashing:** entering a painting. Its scene (`WorldWarpHoleScene`) needs more than the allocator reset, and
  nobody had prepared it. Reproduced by script with the game's own painting warp: 1.0.0 at 165 FPS crashed 1 of 1 with
  the player's exact signature (`f_7000792e68+d4`), 1.4.1 at 240 FPS crashed 1 of 1 with the other player's
  (`f_70009917d0+d2`).
- **The fix that ends the class of bug:** extra draws are **opt-in per scene**. A scene without a tested preparation
  hook runs at the game's own 60 FPS while it shows, then the frame rate comes straight back. An unknown scene now
  costs smoothness for a moment instead of a crash. After: 7 runs at 165 and 240 FPS on both versions, 0 crashes.

*For other projects: when you add frames the game didn't plan for, make "this scene is safe" something you prove per
scene, and make the default the game's own cadence.*

### Frame generation: a swapchain you don't own ([#77](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/77))

With AMD FSR frame generation on, the swapchain handle is AMD's wrapper, not the driver's. Our frame pacing thread
called `vkWaitForPresentKHR` on it, and NVIDIA's driver crashed inside that call (`nvoglv64.dll+dff933`): a black
picture, then a crash. Fix: no present wait while frame generation owns the swapchain. After, at 4K with DLSS and FSR
frame generation: 59 FPS drawn, 118 shown.

### Crashes on quit ([#70](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/70), [#80](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/80))

- **Windows, DLSS frame generation:** the exit path shut NVIDIA Streamline down (unloading its plugins) while the
  render thread was still calling its latency marker: a jump into unloaded code after "window closed".
- **Steam Deck, Gaming Mode:** `exit()` ran the Vulkan layers' clean-up (Steam's shader layer) while our shader
  workers were still compiling.
- **One fix for both:** when the player quits, save what must be saved and leave with a quick exit. Don't tear down
  SDKs and layers while other threads may still be inside them.

### A second render thread: the lost wakeup, and a GPU reset we never explained

"Multithreaded rendering" moves the Vulkan calls to a second thread through a single-producer ring: 148 -> 179 FPS in
busy places at a 240 FPS target.

- **Freezes:** the consumer could miss its wakeup between checking the ring and sleeping. Fixed with sequentially
  consistent loads and stores plus a 2 ms timed wait, and a sync that waits for "at least this position", not "exactly".
- **Steam Deck:** about one scripted run in four ended in a GPU reset ("illegal register access in command stream").
  The Vulkan validation layer's thread-safety checks found nothing. We did not find the cause, so the option is
  **Auto: on with NVIDIA, off with AMD and Intel and on the Deck**.

*For other projects: an optimisation you can't explain a failure of should be off by default where it failed.*

### 120 FPS interpolation: removed

Generating in-between frames ourselves crashed 5 of 5 test runs on game 1.0.0, lost the graphics device while keys
were being rebound ([#40](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/40)) and made shadows
flicker ([#65](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/65)). Native frame rates (every frame
really drawn) do the same job, so the feature was removed rather than patched.

## Memory

Players reported the frame rate falling after a while. On Linux and the Steam Deck memory grew with every kingdom.

- **A megabyte per thread:** each game thread got a 1 MB alternate signal stack that was never freed. Freed now when
  the thread ends (a `pthread` key destructor).
- **Allocator fragmentation:** glibc made an arena per thread and never gave memory back. `M_ARENA_MAX` 4 and a
  `malloc_trim` every 30 s: allocations flat over a session, growth down to about 12 MB a minute and bounded.
- **The driver's share:** NVIDIA's driver keeps about 1 MB for every pipeline prepared ahead. Preparing all of them
  suits a PC with 32 GB; on 16 GB it pushes the game into the page file. Measured on routes through six kingdoms, the
  extra pipelines bought almost nothing: 8,925 prepared (14.7 GB in RAM at the start), 1,883 (11.3 GB) and 304
  (10.8 GB) gave the same frame rate and the same hitches. The warm-up's **Auto** is now 1 GB on every PC
  ([#94](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/94): the old Auto took 32 GB PCs to 10 GB).
- **Shaders built whole when the game sets them up:** each costs about 4 MB of driver memory, and the boot set up
  ~1,450 at once. Only those a scene needs in its first frames ("warm") still are; the rest at their first draw, from
  binaries on disk: about 2 GB less with the same stutter.
- **Pipeline binaries cost NVIDIA memory:** a shader library created from a saved `VK_KHR_pipeline_binary` held
  far more driver memory than the same library from a pipeline cache. Turning binaries off on NVIDIA (the preparation
  fills the pipeline cache instead, as on GPUs without binaries): a new player 6.0 -> 9.1 GB became 4.6 -> 6.1 GB, the
  120 FPS routes 9.3 -> 12.4 GB became 5.8 -> 6.6 GB, with the same frame rate and no more stutter. On an AMD GPU the
  same switch changed nothing (4.1 -> 4.8 GB either way), so other GPUs keep them.
- **One packed shader cache:** the SPIR-V (17,000-54,000 files, 0.9-4.7 GB) went into one zstd file, ~4.3x smaller
  (Odyssey Recomp's design); with only the shaders a run through the game draws compiled before the first start
  (their list merged with ours), a first start's preparation went from 98 s to 51 s and its cache from 2.2 GB to 0.7 GB.
- **Measure, don't guess:** a memory trace every half second from launch (`ODYSSEY_MEM_TRACE=1`) showed which step of
  the start-up each gigabyte came from; three plausible suspects (bulk set-ups, shaders never drawn, preloading) turned
  out to cost almost nothing.
- **A fix we reverted:** joining finished game threads to free them coincided with the game stopping itself with its
  own heap assertion in scripted tours. Reverted until understood.
- **Committed is not resident.** One report showed 22.5 GB committed and 4.1 GB in RAM. Logs and crash reports now
  print both, because players read the bigger one as "RAM used".

## Picture

- **Temporal upscalers on a 16:10 screen** (FSR 2, FSR 3.1, DLSS below native on the Steam Deck): dark smears over the
  ground and half-black buildings. The game's smaller render targets (halves, quarters, an 1/60 grid) didn't divide
  into whole pixels at the scaled size. Fix: snap the upscaler's input to sizes they do divide into (width a multiple
  of 16, height a multiple of 60).
- **FSR 4 on a card it isn't made for** ([#66](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/66)):
  AMD's runtime offered its FSR 4 provider on a Radeon RX 6700 XT, and the picture broke whenever the game changed its
  resolution. FSR 4 is offered on the RX 9000 series only; other cards get FSR 3.1.

## Sound: crackling during loads ([#83](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/83), [#84](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/84))

Before mixing each 5 ms block, the mixer waits for the game's audio thread to answer the previous one, 4 ms at most.
That wait was written as 16 sleeps of 0.25 ms. Linux sleeps that accurately. Windows sleeps 1-2 ms at the least and
far longer when the PC is busy, so whenever the game's audio thread was late (a load), the mixer waited 17, 29 and
90 ms in three measured runs, and the 25 ms device queue ran dry.

What didn't help on its own, measured three runs each: a deeper queue, a higher thread priority, catching up after a
stall. With the faulty wait still there they treated the symptom. The fix bounds the wait by the clock (4 ms, with a precise sleep) and ends it early when the
queue is getting low. Old and new alternating, four runs each (game 1.0.0, a title minute at 120 FPS and a
load): gaps per run 5, 5, 8, 1 with the old wait and 1, 0, 2, 0 with the new one. The gaps that remain come from the
mixer thread itself not being scheduled for a moment during a heavy load (its 4 ms wait measured 16.8 ms once). Running
the mixer at a raised priority on top of the fix removed those too: 0, 0, 0 gaps in three runs, against 2 and 1 without.

*For other projects: never count sleeps to measure time, and log the longest wait of every stage of a real-time loop
before guessing which one stalls.*

## Small bugs with large effects

- **"Show this screen at every start" couldn't be turned off** ([#74](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/74)):
  the start page checked the setting before the settings file was read, so the default always won.
- **A crash report nobody could read:** reports from game version 1.4.1 have no function names. Reports now start with
  a plain-words summary (what, where: game code, graphics driver or the port; the stage, frame rate, upscaler, memory)
  and a one-line signature, so two players' crashes can be matched at a glance.

## How the shader stutter was solved

A Switch game ships its shaders ready for one GPU. On a PC every shader has to be translated (Maxwell -> GLSL ->
SPIR-V) and then compiled by your graphics driver - and the first time an effect appears, that compile is a stutter.
What fixed it, measured on an RTX 4090 and a Steam Deck:

1. **Compile everything ahead of play.** The launcher scans the game's own shader archives (about 32,800 programs),
   translates every one and builds its pipeline pieces, saving them as `VK_KHR_pipeline_binary` data. It asks first,
   shows progress and time left, and keeps what's done if you stop it.
2. **Make sure the game finds what was compiled.** A piece is looked up by its content plus the driver's own key; if
   the compile and the game build anything even slightly differently, every lookup misses and the work is wasted.
   The log reports what was loaded, refused or missing, so coverage can be checked.
3. **Compile again when the cache goes stale.** A GPU driver update makes the driver refuse saved pieces, and a new
   version of the game may translate shaders differently. The cache records the GPU, driver and version it was
   made for and is rebuilt (after asking) when any of them changes - this was the stutter many players still saw.
4. **Never freeze the picture for an effect.** If a piece isn't ready, that one draw waits a frame or two on another
   thread; only real full-screen passes (lighting, composites) hold the frame.
5. **Less work for the driver.** A post-pass inlines the translator's one-instruction helper calls and strips debug
   data from the SPIR-V: about 45% less driver time per shader on a Steam Deck.
6. **Use the whole CPU.** The compile runs on every core but one (137 s -> 110 s from nothing on a 32-thread CPU).

Tested the strict way: game cache deleted, the driver's own cache emptied, full compile, then play - no shader built
while playing, on Windows and on the Steam Deck. A first compile takes about 2 minutes on a fast PC and about 6 on a
Deck.

## Steam Deck optimizations

What took the Deck to a steady 1280x800 at 60 FPS in Gaming Mode at 10 W:

- **Don't wait for the GPU when the game doesn't need to.** The game waited for its whole GPU frame every frame
  (fences); it now continues once the render thread has its data, and waits only where the GPU writes game memory:
  45 -> 58-60 FPS in busy views.
- **Guarded GPU writes**: GPU results copied back into game memory protect their pages instead of forcing a wait; only
  a thread that actually touches them waits. The first Cap Kingdom visit went from 46-49 FPS to ~60.
- **A separate compute queue** for small dispatches that write game memory, so they never queue behind a frame.
- **Map the game's memory straight into Vulkan** (`VK_EXT_map_memory_placed`): about 3 ms less per submission that
  the AMD driver spent re-checking an imported pointer.
- **Loading stutters**: file decompression runs outside the emulated CPU core it was blocking (12-26 ms hitches when
  new things loaded), and a first CPU read of GPU data (the fog) no longer waits for the GPU.
- **Frame pacing**: the game's 60 Hz timer is locked to when the screen really shows each frame (present wait), so
  frames aren't shown twice or skipped.
- **The game's own dynamic resolution** dropped to its floor on the Deck and stayed there; it can be turned off (off by
  default), and its GPU timing now counts busy time only.
- **Power**: CPU and GPU share the Deck's power budget, so CPU time saved is GPU clock gained; at a low power limit a
  manual GPU clock of 1000 MHz keeps 60 FPS (the start screen shows this tip).

More, with how each was measured: [OPTIMIZATIONS.md](OPTIMIZATIONS.md).
