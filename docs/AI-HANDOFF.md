# AI handoff: goals, state and how to work on Super Odyssey PC Recomp

This is the brief for any AI model (Claude Opus, Codex, others) working on this project. Read it fully before
changing anything. The owner provides the hardware (a Windows PC and a Steam Deck) and does the hands-on play testing;
your job is to make measurable progress toward the goals below and to prove it with numbers.

Last updated 2026-11-15, on branch `linux-deck` at commit 6734b81 plus the dev-loop scripts.

---

## 1. The goals (in the owner's words, turned into targets)

| # | Goal | Measurable target |
|---|------|-------------------|
| G1 | **Absolutely zero stutters**, Windows and Steam Deck | No frame over 2x the target frame time in a 10-minute play session (the `[hitch]` log line, `ODYSSEY_HITCH_MS`), including the first visit to every kingdom (shader compiles) and loading zones. |
| G2 | **A Steam Deck mode**: native 16:10 (1280x800), and higher-than-Switch settings, because the Deck is roughly 6-7x a Switch | Deck holds a locked 60 FPS (90 later) in Bonneton (Cap Kingdom town, ~1,500 draws a frame) and in the heaviest kingdoms (Metro, Seaside, Luncheon), at >= the Switch's docked resolution, with 16:10 filling the screen without stretching. |
| G3 | **Runs on very low-end hardware** | A defined floor (e.g. a 4-core CPU with integrated graphics, 8 GB RAM) runs 30 FPS stable. Measure memory and CPU use; don't add cost to the low end for high-end features. |
| G4 | **Less shader compile time, fewer resources**, with high FPS and stable frame times; **all vsync options** | Shorter "preparing shaders" step on first start; a vsync menu: off, on (FIFO), adaptive (FIFO relaxed), mailbox/fast, with a frame cap. Lower RAM / VRAM use. |
| G5 | **1:1 with the Switch version in every aspect** | Picture, physics, timing, audio and behaviour match the console. Any enhancement is optional and off by default unless it is invisible (e.g. resolution). |
| G6 | **No slowdown when frames drop; 30 is the baseline; up to 360 FPS with no animation speed-up** | Game speed stays real time from 30 to 360 FPS. Below 60, the game catches up (no slow motion). Above 60, more frames are drawn but animations and physics keep their speed (interpolation, not faster ticks). |

The current priority is **G2 and G6 on the Steam Deck** (the Deck's frame rate is not good enough yet), with G1 as the
standing rule for every change.

---

## 2. What the project is

- A **static recompilation** of the Switch game's ARM64 code into C (tools/gen.py, tools/lifter.py), built into a
  native program, with the Switch OS (HLE services in runtime/services, runtime/kernel.c) and the **NVN graphics API
  emulated on Vulkan** (runtime/nvn, runtime/nvn/vulkan).
- Game versions: **1.4.1** is the one being tuned (some fixes are 1.4.1-only, e.g. the SZS decompressor override at
  0x97e420 in runtime/hle/odyssey_120.c). 1.0.0 runs too but is not the focus.
- Platforms: Windows (main), Linux / Steam Deck (AppImage), macOS (MoltenVK).
- Threads: the game's emulated cores (gated in kernel.c), a render thread (runtime/nvn/rqueue.c) that turns NVN
  commands into Vulkan, a submit thread and a presenter thread (Linux default), shader workers, a translator.

---

## 3. Where things stand (measured, not guessed)

### Released
- **0.2.10** (public): config page before the game, red-outline black theme, async shaders on by default, DLSS off by
  default, PC stutter fixes (decompressor off the core gate, pipeline-cache autosave only after real compiles).
  The release notes say the Deck isn't playable yet and recommend 60 FPS.

### Local only (not pushed), on `linux-deck`
- **2026-11-15 night session** (details and numbers: [OPTIMIZATIONS.md](OPTIMIZATIONS.md)): present timing +
  vsync timer locked to the display (Linux); fog readback no longer waits on a map's first read; blended full-screen-
  sized draws no longer hold the picture (the owner's ~180 ms capture / box hitches); SPIR-V call inlining + slimming
  (vertex libraries 175 -> 102 ms CPU on a Deck); the shader preparation asked for and run on the launcher page (no
  own window); aspect ratios 16:10 / 21:9 / 32:9 / 4:3 with bars on any screen; VSync, refresh rate, the monitor's real
  resolutions, exact render sizes in the menu; Shake button / key (#39, #27); catch-up clock on Windows too (#13, #42).
  Issue comments posted (no closing): #41 #39 #27 #13 #42 #46 #44.
- **Early fences** (Linux default): the game's `nvnSyncWait` waits only for the GPU work that wrote game memory, not
  for the whole frame. Submissions split after such writes, at most 8 a frame, and none when a submit costs > 1 ms.
- **Placed GPU-owned guest heap** (Linux default, `ODYSSEY_VK_PLACED_HEAP=0` turns it off): the game's heap is
  Vulkan memory mapped at the guest address (VK_EXT_map_memory_placed). This removed ~3 ms per submit that the
  amdgpu driver spent re-checking the old user-pointer import.
- **Catch-up clock** (Linux default, `ODYSSEY_GAMEPLAY_CATCHUP=0` off; runtime/gameplay_clock.h): below 60 FPS the
  game runs up to 3 game ticks per frame so game speed stays real time. At a forced ~35 FPS: 120-122 ticks per 2 s
  with it, 62-74 without.

### Steam Deck numbers
- Scripted Bonneton walk (bench below), defaults: **56-60 FPS** (was 42-46 before the fixes).
- The owner's manual play on the same build: **30-56 FPS** after reaching Cap Kingdom. ~1,100-1,400 draws a frame,
  GPU busy ~1,050-1,250 ms of every 2 s (i.e. GPU-bound at times), render thread ~45-52% of a core, main game thread
  ~50-63%, a decompressor thread up to 68%. **Why the manual run is slower than the scripted one is the open question.**
- **Bench bug, fixed 2026-11-15:** deck-bench.sh exported `ODYSSEY_SYNC_EARLY=` (empty) when `EARLY` wasn't given,
  and the game read "" as off, so early fences were OFF in every Deck bench before that date unless `EARLY=1` was
  given. Bench numbers from before then are without early fences. Now empty settings are dropped, and the game
  treats an empty `ODYSSEY_SYNC_EARLY` as unset. With early fences really on, the scripted Bonneton walk is a locked
  60 (standing still too), also at the owner's 1440p settings.
- **The real cause of the owner's 30-56 FPS (found 2026-11-15):** the owner's save is at the first visit to Cap
  Kingdom (the Cappy-throw tutorial, fog). In that state the game runs **~21 small compute dispatches a frame that
  store into game memory** (mostly `cs 727710c000`, also `727713dc00` and `7277124200`; a few hundred bytes each,
  all in one area of the heap). Each store is a GPU write to game memory, so the early fence must wait for the GPU up
  to it. Splits are capped at 8 a frame, so the game waits for most of the GPU's frame (`SyncWait` ~800 ms per 2 s).
  Reproduce: `SAVE=/run/media/deck/SN01T/sopr-fonttest/save GAME_ENV=ODYSSEY_WARP=,ODYSSEY_LOG_GPU_WRITES=1400
  SHOT=1700 USER_INI=... tools/deck_dev.sh bench <name>`: **46-49 FPS**. The stats line's `gpu writes:` field counts
  them by kind.
  Experiment `ODYSSEY_GPU_WRITE_NOWAIT=1` (don't wait for compute stores): **58-60 FPS**, picture intact. It is
  **not safe to ship as is**: the copy-back (gfast_finish) writes the whole bound ranges back into the heap when the
  GPU gets to it, which could overwrite what the game has written for the next frame, and the CPU might read the
  results.
  **Fixed 2026-11-15 - guarded compute writes** (Linux default with the placed heap; `ODYSSEY_GPU_WRITE_GUARD=0`
  off; runtime/nvn/vulkan/exec.c "Guarded compute writes"): a dispatch storing through the fast-copy table records
  its ranges instead of making the fence wait; the fence ends the submission holding them, and `nvnSyncWait`
  protects their pages (PROT_NONE). A thread touching one faults; the Linux fault handler waits for the GPU's write,
  opens the page and retries (renderer_guard_fault). Pages open by themselves at a present once the GPU is done.
  Measured on the Deck with the owner's save, settings and the same inputs: guard off 39-57 FPS, guard on ~60 (~40
  pages protected and ~1 fault a frame); the town walk stays a locked 60. The stats line shows `guarded N (fence
  splits, fence waits, protected, faults, render-thread)`.
  Known limits: stores outside the fast-copy table (uses_gmem without the map) and vertex/fragment stores still wait;
  a kernel call (e.g. a file read) into a protected page would get EFAULT instead of faulting (not seen); the
  render thread reads such pages without waiting (as it did before).
- **Dynamic resolution fixed (2026-11-15):** with early fences the game read its GPU timing counters before the GPU
  ran the frame and got host-clock translation time, so its dynamic resolution sat at its minimum (960x540 of
  1600x900) on the Deck even with the GPU ~30% busy: "extreme quality loss when moving the camera". The end
  counter's provisional value is now start + the previous frame's real GPU time (exec.c cq_pair_ticks). At
  `resolution = match` the game returns to 1600x900; at the owner's `1440p` it stays at 960x540 because the GPU
  really can't do 2560x1440 at 60.
- **Power target: 60 FPS at 5-7 W, 16:10** (the owner, 2026-11-15). The TDP limit is set by the owner (Steam's
  per-game performance profile on the dev shortcut), not by agents: no system settings changes on the Deck.
  deck-gpu.sh logs the power draw, so check it in each run's gpu.log.
- **16:10 and the Deck's defaults (2026-11-15):** `resolution = 100%` (RES_DISPLAY: the window's own pixels, no floor
  at the Switch's 900 lines) and `aspect = fill` now also handle screens narrower than 16:9. On the Deck: the 3D view
  is 16:10 (the camera's aspect x 0.9, Hor-) at 1280x800 internal = display, and the HUD keeps its 16:9 shape fitted
  to the width (its projection made 0.9 as tall; full-screen panes stretched back vertically). runtime/hle/odyssey_wide.c,
  output_scale.c, exec.c (ui2d viewport). These are the Steam Deck's first-start defaults (settings_steam_deck); the
  menu's Battery saver is unchanged (75% + FSR 1).
- **Direct depth reads (2026-11-15):** a draw sampling the bound depth buffer without writing depth/stencil reads it
  in place instead of a copy (exec.c resolve_feedback; `ODYSSEY_DEPTH_FEEDBACK_COPY=1` restores copies). Deck at 7 W:
  ~1.2 ms less GPU time in the profiled frame, +1 FPS steady state.
- **At 7 W (2026-11-15):** GPU ~300-440 MHz, ~90% busy: first Cap Kingdom visit 42-58 FPS walking, 60 standing. GPU
  time per frame ~17 ms (gpuprof): main scene pass ~4.6-7 ms, 72 one-draw passes ~4.3 ms, 23 passes without draws
  ~1.5 ms (fold clears into the next pass: next), dispatches ~1.5 ms.
- **Blur at 7 W (2026-11-15):** the game's own dynamic resolution drops to its minimum (960x540 of 1600x900 = 768x480
  of the Deck's 1280x800) when the GPU is busy - as a docked Switch would, the Deck's GPU at 7 W being about as fast.
  It is NOT driven only by the counter pair (capping it changed nothing). Rendering the full 1280x800 anyway costs
  ~10 FPS walking (40-48 vs 47-58); scaling every 16:9 target up by the game's drop breaks the picture (the targets
  that don't follow the game's resolution get mismatched: dark smeared ground) - don't retry that. The real fix is
  GPU time per frame (pass overhead, see below), so the game stays at full resolution by itself.
- **Guarded writes cost at 7 W:** one game function (f_7000931b84) touches the compute buffers' pages every frame
  (mostly next to the GPU's results: page-level false sharing) and waits for the GPU: 400-500 ms per 2 s, single
  waits up to 18 ms (stats: `faults N waiting X ms, longest Y`; `ODYSSEY_LOG_GUARD=1` logs where). Guard off is far
  worse (27-32 FPS). **Done 2026-11-15:** the copy back writes the bound buffers' exact bytes (not the merged ranges'
  gaps), and a fault outside every written range is single-stepped (x86-64 trap flag, linux.c step_handler) instead
  of waiting: 2,000-21,000 such touches per 2 s cost microseconds. What still waits: the game's writes ON the result
  buffers (~50-70 per 2 s at 7 W, up to ~17 ms each, because the GPU runs ~a frame behind). A word-exact merge copy
  back (ODYSSEY_GFAST_MERGE=1, off) lets writes through, but then the game's reads around them wait instead - no gain.
  **Done 2026-11-15 - compute queue** (Linux default; `ODYSSEY_VK_COMPUTE_QUEUE=0` off; exec.c "compute queue"): a
  dispatch storing through the fast-copy table whose textures / images are small (shared CONCURRENT, at most 65,536
  texels) and not written in the graphics submission being recorded runs on a compute-only queue, submitted at once,
  waiting only for the last graphics submission that wrote game memory; the next graphics submission waits for it.
  The guard tracks each range's queue (seq / cseq). Deck at 7 W, first Cap Kingdom visit: all ~20 dispatches a frame
  there, guard waits 0 ms (were 300-450 ms per 2 s), heaviest walking stretch 53-60 FPS (was 45-48); Bonneton town
  57-60. Remaining: single ~65 ms hitches (first shader/texture uses, a game-thread one).
- Counter pairs: Odyssey writes start, a counter at a fixed address, then the end (start + 16 bytes): pairs are
  matched by address among the last few counters (exec.c cq_start_of).
- **Console mode (2026-11-15):** setting `console_mode = docked|handheld` (Graphics > Screen shape), Handheld by
  default on a Steam Deck. Handheld = the game's own handheld rendering (1280x720, lighter effects; the window shows
  the 1280x720 corner of a 1600x900 texture - its crop is applied at present and counts as the screen). At 7 W in
  the first Cap Kingdom visit it stays at 1280x720 the whole time (docked falls to 960x540) at 52-60 FPS. Open: blocky
  stair-step edges on silhouettes against the sky (both modes) - likely a low-resolution effect layer upscaled;
  compare with Switch footage before "fixing".
- The Deck has a **15 W budget shared by CPU and GPU**: CPU time saved gives the GPU clock headroom. Cutting CPU work
  (render thread, lifter code quality) therefore raises GPU-bound frame rates too.

### What exists for the goals
- Frame rate: 60 / 90 / 120 (`frame_rate`, runtime/settings.h; 90/120 interpolate with gameplay at 60). **Nothing
  above 120, and no 30 baseline mode yet.**
- Aspect: 16:9 or fill (`ASPECT_16_9`, `ASPECT_FILL`). **No proper 16:10 mode yet** (fill stretches).
- Deck defaults: `settings_battery_saver()` when `plat_is_steam_deck()` (75% resolution + FSR 1).
- Present modes: only environment switches (`ODYSSEY_PRESENT`), **no vsync menu**.
- Handheld mode: `ODYSSEY_HANDHELD=1` makes the game run its handheld mode (lower resolution, as on a Switch in
  handheld), but **the picture is placed wrongly (top-left)**; unfinished.

---

## 4. Priorities (do them roughly in this order)

**The current, owner-approved order is in [ROADMAP.md](ROADMAP.md) (2026-11-15); the list below is the earlier one.**

1. **Deck: the places the owner plays.** The first Cap Kingdom visit is fixed (guarded compute writes, section 3).
   Reproduce with the bench (section 6), with the owner's save (`SAVE=`), in the other places the owner plays,
   compare the stats lines (splits, skipped splits, submit ms, GPU ms, thread CPU). Decide CPU-bound vs GPU-bound per
   place before changing code.
2. **Deck CPU cost.** Biggest lever on a shared-power device:
   - Lifter output quality: keep guest registers in C locals across a function instead of memory, and compute
     condition flags lazily (only when read). Changes in tools/lifter.py need the full build.
   - Render-thread cost per draw (state hashing, descriptor updates); the decompressor's cost.
3. **Deck mode / 16:10**: a proper 1280x800 output (game rendered at a 16:10 viewport, or 16:9 with the HUD placed
   correctly, without stretching), and fix the handheld-mode placement. Profile at the Switch's docked resolution and
   above.
4. **Frame-rate range (G6)**: a 30 FPS cap that behaves exactly like the Switch at 30, catch-up for drops, and
   interpolated rates past 120 up to 360 with animation speed unchanged. Windows needs the catch-up clock enabled and
   tested too (it's Linux-only by default today).
5. **Vsync menu (G4)**: off / on / adaptive / fast (mailbox) plus a frame cap, in the config page and the in-game menu.
6. **Shader compile time (G4/G1)**: shorter warm-up, a better seed cache (data/pipeline-seed.bin), no compile
   stalls on first visits.
7. **Low-end floor (G3)**: memory mode, resolution scaling, and making sure each feature has an off switch.

Every change: G1 (no new stutters) and G5 (1:1) are acceptance criteria, not extras.

---

## 5. Things already tried that were wrong (don't repeat)

- Setting the Deck's GPU clock by hand: not the cause.
- `ODYSSEY_NO_GMEM`'s apparent speed-up: it made draws blank (lost text), not faster.
- Forcing 2x resolution to simulate a slow GPU: the game lowers its dynamic resolution instead. To force ~35 FPS use
  `ODYSSEY_TEST_COUNTER_DELAY_MS=22`.
- ASTC decoding and the watchdog were not behind the 200 ms frames (that was a GPU-memory read in kbprompts.c).
- "Game Mode vs Desktop Mode" on the Deck: not the cause.
- Renaming the test exe on Windows: the game library binds to `SuperOdysseyRecomp.exe` by name; keep the name.

---

## 6. How to work: build, test, measure

### Windows (PC)
- Build: docs/BUILD-windows.md. From Git Bash, put the msys tools on PATH **only for make, in a subshell**:
  `(M=<msys root>; PATH=$M/ucrt64/bin:$M/usr/bin:$PATH make ...)`. Exporting that PATH for the game's launch breaks
  its environment.
- Test with the copy in `sopr-wintest`, never the owner's real install or saves.
- Scripted runs need `ODYSSEY_NO_CONFIG=1` (otherwise the config page waits for input).

### Steam Deck: the fast loop (seconds, not a full rebuild)
From the PC (Git Bash), with `DECK=deck@<ip>` in the git-ignored tools/release.local.sh:

```bash
tools/deck_dev.sh                      # copy the changed sources to ~/sopr on the Deck, rebuild only the runtime
tools/deck_dev.sh bench <name>         # ... and run the scripted Bonneton walk, print FPS / worst frame
tools/deck_dev.sh full                 # full AppImage (needed once, and after any lifter/generator change)
```

- Only files whose content changed are copied, so make stays incremental. A runtime-only change rebuilds in seconds.
- The quick build runs the AppDir directly (`build/linux/x86_64/AppDir/AppRun`); no AppImage packing.
- Bench options (environment): `FULL=1`, `USER_INI=/home/deck/sopr/bench/user.ini`, `WARP=world:<Stage>@<frame>`,
  `INPUT=...`, `SHOT=<frame>`, `HANDHELD=1`, `HITCH_MS=`, `PRESENT=`, `EARLY=`, `NOGMEM=` and more (see the header of
  tools/linux/deck/deck-bench.sh). Results and the game log land in `~/sopr/bench/<name>/` on the Deck.
  `SAVE=<folder or .tar>` starts from another save (copied, never written); the default is the completed save, which
  does NOT show first-visit scenes. `GAME_ENV=ODYSSEY_A=1,ODYSSEY_B=2` passes any game setting
  (`ODYSSEY_WARP=` with nothing after it turns the bench's warp off).
- Diagnostics: `ODYSSEY_SYNC_TRACE=1` (the game thread's frame: submits, fence waits, present throttle),
  `ODYSSEY_LOG_GPU_WRITES=<frame>` (each compute dispatch storing to game memory in that frame).
- **Testing is done in Gaming Mode** (the owner's choice, 2026-11-15). The owner's Steam has a non-Steam shortcut to
  `/home/deck/sopr/tools/linux/deck/play-dev.sh`: played from Steam it runs the newest quick build (the AppDir) on the
  test copy with stats logging (`/run/media/deck/SN01T/sopr-0.2.9.1/logs/dev-session-*.log`), so a change can be
  played minutes after it's made, without an AppImage. Scripted runs from the PC go through the same shortcut:
  `LAUNCH=steam tools/deck_dev.sh bench <name>` leaves the run's settings in `~/sopr/bench/next-run.env` (used once,
  only if under 2 minutes old) and starts the shortcut with `steam -ifrunning steam://rungameid/<id>`. Without
  `LAUNCH=steam` a bench starts the game directly (needs Desktop Mode).
- A failed quick build stops deck_dev.sh, so a bench never runs an old program; the build log is
  `~/sopr/build/dev_quick.log` on the Deck.
- tools/linux/deck/deck-threads.sh and deck-gpu.sh sample per-thread CPU and GPU busy/clock while it runs.
- The Deck test copy of the game is `/run/media/deck/SN01T/sopr-fonttest`. Never use the owner's real Deck install.
- On the Deck: no sudo, no installs, no system setting changes. Start long jobs with `setsid nohup ... < /dev/null &`
  or `systemd-run --user` (plain nohup over SSH gets killed).

### What to read in a log
- `[vk] t=... frame N  X fps max Y ms ...` every 2 s: FPS, worst frame, draws, GPU ms, heap binds,
  `splits N (skipped M, a submission X ms)`.
- `[hitch]` lines (`ODYSSEY_HITCH_MS=<ms>`): a slow frame broken into wait / acquire / present.
- `[display] 10s: N shown, D dropped, H held ...` (Linux, present wait): what the screen actually showed. Held = an
  image shown for a second refresh (a visible stutter); dropped = replaced before shown. "Flat graph" means held 0
  outside real game hitches. `timer phase` stays put when the vsync timer is locked to the display (vi.c).
  `ODYSSEY_LOG_PRESENT_GPU=1` adds when the GPU finished each present's work.
- Thread names (Linux): `g<id> c<core> p<prio>` for game threads, `render`, `shader worker`, `translator`.

### A Linux PC
The owner can provide a Linux environment on the PC. Useful for RADV / Mesa profiling with a real GPU and faster
builds than the Deck; ask for it when a problem needs it.

---

## 7. Rules (non-negotiable)

- **Everything stays local.** No git push, no GitHub release, no issue/PR comments unless the owner says so in that
  conversation. Releases go through `tools/release.sh` (it tests before publishing).
- Never touch the owner's real game install or saves on the PC or the Deck; use the test copies above.
- Never print or log secrets from the owner's files. Keep the owner's personal details out of commits and outside
  services.
- Don't change system or security settings on any machine.
- Don't edit another agent's checkout (e.g. Codex's); read it and bring the changes over.
- Defaults that differ by platform (Linux-only today) must stay switchable by an environment variable, so a
  regression can be confirmed or ruled out in one run.
- Report results honestly: numbers from a run, which build, which place in the game. "Should be faster" is not a
  result.

---

## 8. Definition of done for a performance change

1. Before/after numbers from the same bench run (same place, same settings), on the device that matters.
2. No new `[hitch]` frames and no visual difference from the Switch (screenshot the same spot: `SHOT=<frame>`).
3. Windows still builds and runs; the change is off or neutral where it wasn't measured.
4. Committed locally with a message saying what was measured.
