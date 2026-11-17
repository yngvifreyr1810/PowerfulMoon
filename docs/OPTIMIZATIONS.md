# Performance techniques in Super Odyssey PC Recomp

What made this static recompilation (Switch ARM64 -> C, NVN emulated on Vulkan) run smoothly, why each one works,
how it was measured, and the switches that turn it off. Most of it applies to any recompiled or emulated console
game - Switch or not: the problems are the same wherever a game written for unified memory and a fixed-function
display meets a PC driver.

Everything here was measured, not assumed: numbers come from scripted runs on a Steam Deck (Gaming Mode, 10 W) and a
Windows PC (RTX 4090, 144 Hz monitor) unless stated. The tools are at the end.

## 1. Frame pacing: measure what the screen shows, not what the game submits

**Problem.** A game's own frame times can be perfectly flat while the screen still stutters: a timer a hair off the
display's refresh, or a present that lands right at the compositor's latch point, shows one image twice and drops
another now and then.

**Technique.** `VK_KHR_present_id` + `VK_KHR_present_wait`: tag every present with an id and have a thread wait for
each in turn (`runtime/nvn/vulkan/pacing.c`). Two completions within a few ms: the first image was replaced before it
was shown (dropped). A gap of 1.5 refreshes or more: the previous image was shown again (held). Log shown / dropped /
held and the spacing's p50 / p99 / max every 10 s; draw the performance overlay's frame-time graph from these, so a
held frame is the spike the eye sees.

**Lock the game's vsync timer to the display.** The emulated 60 Hz timer (`runtime/hle/vi.c`) takes its period from the
measured display spacing (filtered) and is nudged (at most 0.25 ms a tick) until its ticks fall a fixed offset after
the reported screen updates. Unlocked, its phase is wherever it started and creeps (~0.03 ms/s on a Deck), so every
few minutes it sits at the latch point. Deck, 50 s walk: images held 5-9 unlocked, 0-2 locked at a 9 ms offset (each
remaining one a real game hitch); offsets 1 / 5 / 13 ms gave 2 / 7 / 6. Default on Linux and, since 2026-11-16, on
Windows too (`ODYSSEY_VSYNC_LOCK`, `ODYSSEY_VSYNC_LOCK_MS`; NVIDIA and AMD drivers have present wait there).

**A refresh rate that isn't a multiple of the frame rate can't be smooth.** 60 FPS on a 144 Hz monitor alternates
13.9 / 20.8 ms on screen - a judder no VSync mode removes. Draw at a rate the refresh divides instead: Frame rate
"Match display" picks the highest one up to 120 (144 Hz -> 72, 240 -> 120). Borderless on a 144 Hz monitor, RTX 4090:
60 FPS p99 19 ms between images shown; 72 FPS every image 2 refreshes, p99 16 ms, 0-5 held per 10 s. (144 FPS itself
held only ~113 FPS in busy stretches on that PC: a rate the machine can't hold is worse than a lower even one.)

**Draw every frame, don't generate them.** Rates above 60 (72 ... 240) draw the game every vsync with Mario and the
camera placed at that vsync's moment between two 60 Hz gameplay ticks (`runtime/hle/odyssey_120.c`): real frames, no
interpolated images, gameplay logic untouched (it counts frames). 240 FPS on an RTX 4090: a camera pan moves 5-6 px
every frame.

**Draw the HUD after upscaling.** Upscalers (DLSS / FSR / XeSS, or a lower internal resolution) take the game's final
picture, HUD included: the HUD came out blocky and smeared. Marking the game's UI draws (here: hooking its UI library's
draw entry and recording a marker into the command stream) lets the renderer send them to a layer at the output
resolution, composited after the upscale (`runtime/nvn/vulkan/exec.c`, "HUD layer"). When the game draws scene content
over UI it drew earlier (a 3D model over a menu background), the layer is flattened into the picture first, keeping the
game's order.

## 2. Don't make the game wait for the GPU when it doesn't need to

**Early fences** (`nvnSyncWait`, `runtime/nvn/objects.c`): once the render thread has translated the fenced work, the
game's memory it read has already been copied (uniforms, vertices, textures), so the game may continue before the
GPU has run it - except where the GPU writes game memory. The game was waiting for its whole GPU frame every frame
(Deck: game 8 + translation 7 + GPU 7 ms = 22 ms, 45 FPS in busy views; with early fences 58-60).

**Guarded GPU writes**: compute results copied back into game memory would still force the wait. Instead their pages
are protected (`PROT_NONE`) at the fence; a thread that touches one waits for the GPU there (fault handler), a touch
outside the written byte ranges is single-stepped (x86-64 trap flag) instead of waiting. First Cap Kingdom visit on a
Deck: 46-49 FPS -> ~60.

**A compute-only queue** for small dispatches that store to game memory: submitted at once, on their own timeline, so
they never wait behind a frame's graphics work. Guard waits 300-450 ms per 2 s -> 0.

**Stop the game's first reads from waiting.** Any "read back one texel" path should keep a copy a frame or two old and
never flush on the first read: Cap Kingdom's fog-height query waited ~25 ms of render thread and ~50 ms of game thread
on its first frame (no copy yet). Returning "not yet" for that frame removed the hitch.

## 3. Shader compilation: stutters, preparation time, coverage

**Never hold the picture for an effect.** If a draw's pipeline isn't built yet, skip the draw (it appears a frame or
two late) - except for real full-screen passes (lighting, composite), where a skipped draw shows a black or broken
frame. Classifying "full screen" by size alone catches sprites close to the camera (dust from a broken box, a
capture's effect): those are blended over the scene and are safe to skip. Holding for them froze the picture 100-300 ms
(the "very hard" effect hitches); now blended or scissored full-screen-sized draws don't hold
(`ODYSSEY_HOLD_OVERLAYS=1` restores).

**Make the driver's job smaller.** glslang at -O0 turns every tiny helper (`as_f(x)` = `uintBitsToFloat`) into a real
call: a temporary variable, a store, the call, a load - ~1,250 per median vertex shader, half its instructions. A
post-pass over the SPIR-V (`runtime/nvn/vulkan/shaders.c inline_trivial`, `spv_slim`) replaces each call of a
one-instruction function by that instruction and drops debug names and the then-dead temporaries. Library build CPU
on a Deck: vertex 175 -> 102 ms, fragment 60 -> 13 ms; RTX 4090 full preparation 125 -> 97 s. Validated with
`spvValidate` (Vulkan 1.3) on 15,097 modules. spirv-opt at full optimisation gets similar driver savings but costs
~620 ms per program on a Deck itself - too slow. The pass keeps the SPIR-V cache keys (they hash the GLSL source), so
nobody has to rebuild their cache.

**Preparation (precompile)**: every shader of the game's archives (32,773 programs) translated, then its pipeline
libraries built and saved as `VK_KHR_pipeline_binary` data, before the first start. Things that matter:
- *Coverage is only as good as the keys.* A library is found by its content (SPIR-V key, layout, state) plus the
  driver's key for its create info. If the preparation process and the game build anything differently, every lookup
  misses and the whole preparation is wasted. Check it: the log's `binaries N loaded ... absent` - after a full
  preparation the game should load almost everything (RTX 4090: 1,231 loaded, 2 absent).
- *Ask, in the launcher, before doing it* - with a progress bar, a time estimate and a way to stop (it keeps what's
  done).
- Know where the time goes: on a Deck, scan 43-66 s, translation 159-241 s, library builds 215-539 s.
- *Prepare again when the cache goes stale.* A driver update makes the driver refuse saved pipeline binaries, and a new
  version may translate shaders differently; a cache marked "prepared" once was trusted forever and every effect built
  while playing. Record what the cache was made for (GPU vendor / device, driver version, program version) and ask to
  prepare again when it differs (`runtime/nvn/vulkan/device.c prep_identity` - read from a short-lived Vulkan instance,
  before the renderer starts).
- *Use the whole CPU.* The driver's compile is CPU-bound: every core but one instead of at most 16 took a truly cold
  preparation (our cache deleted, the driver's disk cache redirected to an empty folder) from 136.7 s to 110.0 s on a
  32-thread CPU.
- *Test it cold.* Delete the cache, empty the driver's cache (NVIDIA: `__GL_SHADER_DISK_CACHE_PATH` to an empty folder
  for the test process; Mesa: `MESA_SHADER_CACHE_DIR`), prepare, then play: a run on a warm cache hides what players
  see after an update.

## 4. Game-side hitches from emulated threading

Recompiled games often emulate the console's cores with "core gates" (one guest thread per emulated core at a
time). A loading thread decompressing (SZS) holds the core; any of its system calls give the core away and take it
back mid-decode, stalling the main thread on that core for 12-26 ms. Running pure computation (decompression) off
the gate for its whole duration (`kernel_ungated_begin/_end`) took gate waits in play from 11-15 to 0.

## 5. Keep game speed real time when frames drop

The game advances one tick per frame. Below 60 FPS it runs in slow motion unless the host runs extra ticks per frame
(`runtime/gameplay_clock.h`, `ODYSSEY_GAMEPLAY_CATCHUP`): at a forced ~32 FPS, 120 ticks per 2 s with it, 60 without.
On by default on every platform.

## 6. Resolution, shape and the game's own dynamic resolution

- The game's dynamic resolution (`al::GraphicsQualityController`, a Wait -> Reduce -> Recover state machine) drops the
  internal resolution to its floor on weak GPUs and never raises it; turning its Reduce step into a no-op (found from
  the RTTI name in both game versions) keeps the full resolution. A setting, off by default on a Steam Deck.
- The game decides from its GPU timing counters. With early fences a frame is several submissions with the GPU idle
  between them; counting those gaps made a fast camera turn look like heavy GPU work (#51). The counters now carry
  busy time only (per-submission begin / end timestamps; ODYSSEY_LOG_QUALITY=1 logs its Reduce steps).
- Mip chains (blur, bloom, reductions) render level N while sampling the same texture for level N - 1: a sampled view
  of levels 0..N-1 avoids the feedback copy each draw used to make (~7 a frame; +6% FPS on an AMD iGPU).
- Any picture shape on any screen: the game's camera and HUD layout take a shape factor (ratio / 16:9); the image is
  fitted into the window with bars where the shapes differ (21:9 on 16:9: bars above and below). Handheld 16:10 needs
  the HUD's rows scaled and full-screen panes stretched back.

## 7. Steam Deck specifics

- Power is shared (15 W total, or less with a TDP limit): CPU time saved is GPU clock gained. A manual GPU clock of
  1000 MHz at 10 W holds 60 FPS where automatic clocks don't.
- The placed guest heap (`VK_EXT_map_memory_placed`): the game's memory as Vulkan memory at its guest address removed
  ~3 ms per submission that amdgpu spent re-checking a user-pointer import.

## Tools

- `tools/deck_dev.sh bench <name>`: sync, incremental build and a scripted run on the Deck (Gaming Mode with
  `LAUNCH=steam`); options for saves, input scripts, screenshots along a route (`SHOTS=`), another data folder for cold
  caches (`DATA=`), extra environment (`GAME_ENV=`).
- `tools/linux/deck/effects-route.txt`: a scripted route through Cap Kingdom full of effects.
- Logs: `[display]` (what the screen showed), `[hitch]` (a slow frame broken down), `[shtime]` (shader builds over
  `ODYSSEY_LOG_SHADER_TIME` ms, with CPU time; held frames with the draw's target and blend), `[gate]` (core gate
  waits), `ODYSSEY_GPUPROF_AT=<frame>` (GPU time per pass).
- Sampling profiler (Linux): `ODYSSEY_SAMPLE_AT`, `ODYSSEY_SAMPLE_THREAD_NAME=render|present|<name>`,
  `ODYSSEY_SAMPLE_STACK=1`, then `tools/linux/deck/stacks.py` (names frames in the binary and the game library).
- `ODYSSEY_DUMP_GLSL=<folder>`: every GLSL source compiled (not from the cache) - to diff the preparation against the
  game.
