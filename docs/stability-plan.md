# Stability plan: crashes, frame drops, steady play (2026-11-17)

Built from every log, crash report and text file attached to the open issues (read 2026-11-17, after 0.4.0), and run
through the loop in the owner's `WORKFLOW-stability-plan.md` (measure first, one change behind one switch, automatic
gates, owner play card, then ship). Enhancements (#62, #64, #73, #48, #82, #81's shortcut) are out of scope for now.

Test rules for these sessions: the PC's test copies only (`I:\Switch Games\sopr-wintest`, `sopr-100`), the game's
volume at 0, both monitors kept dark while tests run, nothing pushed or posted without the owner.

## What the issues say

| Issue | Version / hardware | Evidence | Reading |
|---|---|---|---|
| #85 painting crash | 0.4.0, game 1.4.1, RTX 5070 Ti, 240 FPS | `c0000005 accessing 0x20` in `f_70009917d0+d2` (1.4.1 has no symbols), 30 min session | Same signature as #72's follow-up below, on 1.4.1. |
| #72 (reopened by its reporter) | 0.4.0, game 1.0.0, RTX 2080 SUPER, 165 FPS | entering the Lake Kingdom pond painting: `WorldWarpHoleScene::drawMain -> DepthShadowMapDirector::drawToMask -> DynamicTextureAllocator::alloc_` `0x20` | The extra-frame allocator reset (f39a0c0) misses the painting-warp scene. On 1.4.1 the log even says `no DynamicTextureAllocator::calc; extra draws without it`. |
| #71 | 0.3.0, launcher | `DynamicTextureAllocator::alloc_+0x378` under `StageScene::drawMain` | Same allocator family; 0.4.0 fixed the StageScene path; reporter asked to retest. |
| #77 | 0.3.1, NVIDIA, 4K, FSR frame generation | crash in `nvoglv64.dll+dff933` | **Fixed locally (57d58cb):** the pacing thread called vkWaitForPresentKHR on the frame-generation swapchain wrapper. Reproduced and verified. |
| #83 crackling + "12 GB" | 0.4.0, game 1.0.0, i5-12600K, RTX 4070 Ti, Match display 90 Hz | menu music and in-game sound crackle; recording thread on; title at 90 FPS, play at 60 | Audio queue only ~25 ms ahead of a 10 ms WASAPI device; the mixer thread runs at normal priority beside more busy threads in 0.4.0. No underrun counter exists yet, so unproven. "12 GB" is likely committed memory, not RAM in use (see B1). |
| #84 audio distortion | 0.4.0 | "slight distortion when transitioning between scenes in cutscenes" | Same family as #83: loads stall the threads that feed the mixer. |
| #79 crash NDC night to day | 0.3.1, 1.0.0, RTX 3060 Ti, 32 GB | crash after long session, garbage NVN handles | Reporter: no crash on a rerun ("could be a long-running session"). Memory growth (fixed in 0.4.0) fits. Close if it stays quiet. |
| #21 Linux freeze | 0.4.0, RX 7800 XT (RDNA3), RADV | kernel: `ring gfx_0.0.0 timeout ... thread render`, MODE1 reset, VRAM lost; Early frame hand-off off didn't help | A GPU hang in the graphics queue on RDNA3 only. Needs `RADV_DEBUG=hang` / devcoredump from the reporter; can't reproduce on NVIDIA or the Deck. |
| #66 corruption | 0.3.1, RX 6700 XT (RDNA2) | gone with **FSR 4** off; FSR 4 provider 4.1.2 runs on RDNA2 | FSR 4 isn't meant for RDNA2. Also: tutorial video not shown, blocky shadows (0.3.1 had no Shadow quality). |
| #75 Wooded Kingdom FPS | 0.3.x, RTX 2060, 16 GB | ~22 GB committed on a 16 GB PC, thousands of skipped draws while shaders build | Memory pressure + shader building, not the goo (the goo runs 110-120 FPS on the owner's PC). |
| #61 FPS 60 -> 40 over time | — | no log | Matches the memory growth fixed in 0.4.0; waiting for the reporter. |
| #42 / #38 frame drops (Lake sand, Secret Flower Field, Cascade waterfall) | 0.2.x | no current logs | Retest on 0.4.0 (asked). Routes for the FPS gate. |
| #55 crash after 786 s | 0.2.11, Radeon iGPU, launcher | only the shader-prep log | Waiting for the game log. |
| #43 no sound on the Deck | 0.2.11 | PulseAudio opened, 5.1 mix | Try Stereo; needs the Deck. |
| #68 gyro on the Deck | — | Steam's virtual pad has no sensors | Read the Deck's own gyro; needs the Deck. |
| #37 map flags (Moon Kingdom), #69 wardrobe | — | not reproduced on 0.4.0 (PC, 1.0.0 and 1.4.1, 60 and 120 FPS) | Waiting for the reporters. |
| #40 key binding crash | — | reporter: "key bindings work fine now in 0.4.0" | Close. |

## Work, in order

### A. Crashes
1. **A1 Painting-warp crash above 60 FPS (#72, #85; maybe #71).** Reproduce on 1.0.0 (`sopr-100`, save-lake) at 165
   FPS, then 60 FPS. Then make the extra frames above 60 **opt-in per scene**: a scene without a tested allocator reset
   (WorldWarpHoleScene now, any unknown scene later) draws only the game's own 60 frames, so an untested scene costs
   smoothness, not a crash. On 1.4.1 (no `DynamicTextureAllocator::calc` found) find the function by signature, or keep
   extra frames to scenes proven safe. Switch: `ODYSSEY_EXTRA_DRAW_SCENES=all|known`. Gate: 20 painting entries at 165
   and 240 FPS on both versions, zero crashes.
2. **A2 Frame generation (#77): done locally**, needs the gate: every upscaler x frame-generation pair, toggled 20 times.
3. **A3 Crash signatures:** add the current scene's class name to the crash report header, so each report names its
   scene (the reports above only show it in the guest call chain, and 1.4.1 has no names).
4. **A4 Linux RDNA3 hang (#21):** ask for `RADV_DEBUG=hang` output and the devcoredump; meanwhile list what differs on
   RDNA3 (compute queue, placed heap, pipeline binaries) and offer one switch per item.
5. **A5 Nightly tour:** the kingdom tour plus paintings, wardrobe and shops, at 60 and 165 FPS, 1.0.0 and 1.4.1, the
   upscalers rotated; every crash signature counted.

### B. Memory and sound (steady play)
1. **B1 Honest memory numbers:** show resident memory next to committed in the overlay and logs (players read 12-22 GB
   "committed" as RAM in use, #72, #83).
2. **B2 Audio underruns, measured:** count the times the device queue runs empty (per 10 s in the log). Then A/B:
   mixer thread at raised priority (`plat_thread_realtime`) and a queue of 3 device periods instead of 2, behind
   `ODYSSEY_AUDIO_QUEUE` / `ODYSSEY_AUDIO_PRIO`. Gate: zero underruns in a title-screen minute and in a cutscene
   transition, with the recording thread on and off; latency added at most ~10 ms.
3. **B3 Leak soak:** 30 kingdom warps; resident and committed memory at warp 30 within ~3% of warp 5.

### C. Frame drops
1. **C1 Routes:** Cap town, Lake Kingdom sand (#42), Secret Flower Field (#42), Cascade waterfall (#38), Wooded goo
   (#75), Metro crowd. Gates: average FPS, worst frame, `[hitch]` lines, held frames, skipped draws.
2. **C2 Low-memory PCs (#75, #61, #79):** run the routes with a 16 GB memory cap (Windows job object) to see whether
   0.4.0's warm-up Auto keeps committed memory under the cap; if not, lower it further below 24 GB.

### D. Picture on AMD (#66)
1. FSR 4 only on GPUs it's made for (RDNA 4); other GPUs fall back to FSR 3.1, as NVIDIA already does.
2. Tutorial videos and blocky shadows: ask for a 0.4.0 retest with Shadow quality High; a 0.4.0 log if the video panel
   stays empty.

## Results so far (2026-11-17 night, local commits only)

- **A1 painting crash: fixed, verified on 1.0.0.** Reproduced with a new test warp, `ODYSSEY_WARP=hole:Go@<frame>`
  (the game's own `GameDataFunction::tryChangeNextStageWithWorldWarpHole`, as entering a painting does; 1.4.1 address
  added to `odyssey_v141.h`). Lake Kingdom pond painting, 1.0.0, 165 FPS, extra draws everywhere
  (`ODYSSEY_EXTRA_DRAW_SCENES=all`): crash 1 of 1, `c0000005 accessing 0x20` in `f_7000792e68+d4` - the #72 report's
  exact signature. 1.4.1, 240 FPS: crash 1 of 1, `f_70009917d0+d2` - #85's exact signature. With the fix
  (`odyssey_native_hz()` returns 60 while the last gameplay frame came from a scene without extra-draw preparation):
  1.0.0 at 165 / 165 / 240 FPS, 0 crashes in 3, the painting at 60 FPS and the next kingdom back at 120 at once.
  1.4.1 with the fix (display on): 165 / 165 / 240 / 240 FPS, 0 crashes in 4, Lake Kingdom at 120 FPS before the
  painting, the painting at 60, Luncheon Kingdom after. **Gate passed on both game versions (7 runs).**
- **A3 crash reports** now carry `Stage:` and `Frame rate: setting N, drawing M a second`.
- **B2 audio, measured, not fixed yet.** A first counter (queue empty before a block) over-counted; the log now
  counts real gaps (SDL's get-callback: the device's pulls it found short) and, per 10 s with gaps, the mixer's longest
  waits (`longest: between blocks, waiting for the game, for the lock, mixing`). 1.0.0, Match display, a title minute +
  a load, 3 runs each, real gaps: queue of 2 periods (25 ms, as shipped) 0 / 6 / 5; 3 periods 9 / 4 / 2; 3 + catch-up
  7 / 0 / 0; raised mixer priority no better. No setup is clearly better, so the defaults stay as shipped (switches
  kept: `ODYSSEY_AUDIO_QUEUE`, `ODYSSEY_AUDIO_PRIO`, `ODYSSEY_AUDIO_CATCHUP`). The gaps come at the title's load and at
  the Continue load whatever the queue depth: a long stall of the mixer, not a short queue. **Next:** run the new
  longest-wait line to see which wait it is (the renderer lock the game's audio thread holds is the first suspect).
- **Test environment, learned:** with the monitors powered off the NVIDIA driver reports no active display and every
  present waits ~250 ms: the game runs at 4-10 FPS and high-FPS paths never happen, so such runs prove nothing. Keep
  the monitors powered and cover them with the black overlay (`black_overlay.ps1`: a black always-on-top window on
  every screen, display kept awake) instead.

- **New crash signature, seen once (2026-11-17, build 4346029 + the audio fix, game 1.0.0, Windows):** at the first
  frames, `exception c0000005 accessing 0x7af5cdf420` in `f_700420a2f4+478`, called from
  `nn::hid::GetTouchScreenState` <- `sead::ControllerMgr::calc`. The address is inside the hid shared memory the game
  mapped (x8 = 0x7af5cdf400, the touch screen's block). One start in roughly a hundred that day; the start-up log up
  to it matches a good run line for line. Not from PR #86 (it doesn't touch this path). Suspects, unproven: the page
  protected or decommitted by something else (the guarded GPU-write pages, a neighbour's unmap), or the game's random
  choice of mapping address landing somewhere not committed. **Next:** log the address and protection of every
  MapSharedMemory, and VirtualQuery the faulting page in the crash report; run 200 starts by script.

## Results, 2026-11-17 evening (after 0.4.2, local commits only)

- **Start-up crash in `nn::hid::GetTouchScreenState`: cause found and fixed (8bfb6bf).** The crash's registers put the
  touch screen's state at `0x7094004400`: the hid shared memory was mapped at `0x7094004000`, inside the runtime's HLE
  heap (`0x7092000000`, 224 MB of host-owned data the guest can see), which QueryMemory reported as **free**, along
  with everything outside the guest window. The SDK maps shared memory at a random free address, so roughly one start
  in 40-70 could land there; host data then overwrote the touch lifo's header and the game read past it. Now the HLE
  heap and the HLE call range are listed as inaccessible (type 0x10, not committed up front) and addresses outside
  the window are reported inaccessible. `ODYSSEY_LOG_MAPS=1` logs every mapping and what QueryMemory says about the
  crash's address (old: `7090100000+f6ff00000 type 0`, free; new: `7092000000+e000000 type 10`). A mapping onto the
  runtime's memory or outside the window is always logged. 80 scripted starts (40 old map, 40 new): 0 crashes; the 160
  mappings all landed in `0x7000000000-0x7220000000`. `ODYSSEY_QUERY_OLD=1` restores the old map for A/B.
- **B1 honest memory numbers: already there.** The `[status]` line, the crash report summary and the overlay's "game"
  figure show memory in RAM (resident) apart from committed.
- **B3 leak soak: passed.** 30 kingdom warps (Cascade, Sand, Lake, Wooded, Metro, Seaside x5) in one 23-minute
  session, 60 FPS, this PC (95 GB RAM, warm-up Long): in RAM 16.8 GB at warp 5, 17.1 GB at warp 30 (+1.9%); committed
  27.5 -> 27.9 GB (+1.7%). 0 crashes in 31 arrivals; lowest 2 s 51 FPS.
- **C2 16 GB PCs: the level, not growth, is the problem.** Same soak with a 16 GB PC's settings (warm-up Normal, preload
  budget 4 GB), 12 warps: 12.5 GB in RAM at the first arrival, 15.1 GB after six kingdoms, 25.6 GB committed - a
  16 GB PC swaps (#75, #61). Breakdown at 10 s: guest memory 3.2 GB, heaps 0.5 GB, the rest (~8 GB) the driver's
  objects for the ~14,300 shader programs set up before play. **Low memory** (Auto only below 12 GB today) on the
  same route: 4.4 GB in RAM at start, 5.9 GB after the warp (programs set up 60 -> 420); cost: one 340 ms frame on
  arriving in Sand Kingdom (Normal: worst 92 ms). **Open decision (owner):** Auto -> Low memory below 20 GB of RAM,
  i.e. on 16 GB PCs.
- **C1 routes at 120 FPS: clean.** Cascade, Sand, Lake, Wooded, Metro, Cap, walking and turning the camera
  (NVIDIA, 1600x900 window): averages 113.9-117.0 FPS, 0 skipped draws, 4 frames over 20 ms in play (worst 42 ms,
  Metro's crowd, 3 frames in a row), 0 crashes.

- **RAM, first cut (owner: keep 16 GB PCs on Normal, make Normal leaner).** A half-second memory trace from launch
  (`ODYSSEY_MEM_TRACE=1`) shows where 16 GB settings reach 12 GB: the pipeline warm-up +2.5 GB (2.5-5 s), then +7 GB in
  2.5 s while the game sets up ~14,300 programs at boot - ~1,450 of them (drawn before, anywhere) built whole at once,
  ~4 MB of NVIDIA driver memory each. Now only *warm* programs (drawn in a scene's first frames) are built whole; the
  rest get their libraries at their first draw (`ODYSSEY_PREWARM_KNOWN` now off by default). 16 GB settings, Cap ->
  Sand -> Metro -> Wooded, 2 runs each with an empty driver cache: 11.7 -> 10.3 GB at start, 13.6-14.2 -> 11.7-12.3 GB
  after three kingdoms, worst frames and >50 ms frames unchanged. This PC's settings, the 120 FPS routes: 1.1-1.3 GB
  less, 105.4 vs 105.5 FPS, 4 in-play hitches both, arrival hitches median 26 vs 25 ms, worst 91 vs 94 ms. Measured
  and ruled out: only programs drawn before set up at all (`ODYSSEY_PREWARM=3`, -0.4 GB), no bulk set-ups
  (`ODYSSEY_PREWARM_BULK=0`, nothing), warm-up off (-1.6 GB but 250-300 ms arrival freezes), Low memory (-7 GB at start
  but 330 ms freezes, and 8 GB after three kingdoms anyway). Note: `ODYSSEY_LOG_MEM=1` costs ~65 ms a stats line
  (heap walk) - no frame-time measurements with it.

- **Shader preparation: only drawn shaders compiled for the GPU (#94, after Odyssey Recomp 0.2.6 / 0.2.7).** Every
  program is still translated, but the driver compiles only those drawn before (the shipped list, now merged with
  Odyssey Recomp's 197-stage list: 1,737 -> 3,470; and the player's own), the compute programs and the whole effects
  archive. A new player's first start (empty game and driver caches, 16 GB settings, Cap -> Sand -> Metro -> Wooded),
  all programs vs drawn only: 98.2 s -> 59.4 s before the start, 31,570 -> 6,833 programs compiled, cache 2.2 -> 1.7 GB,
  RAM 8.4 -> 11.7 vs 8.2 -> 11.4 GB, worst frame 423 vs 460 ms, frames over 50 ms 9 vs 10, 60.2 vs 60.1 FPS.
  `ODYSSEY_PRECOMPILE_ALL=1`: all, as before. Opt-in for tests: `ODYSSEY_PRECOMPILE_TRANSLATE=known` (translate only
  those too: 44.0 s, cache 1.3 GB, the same stutter on this PC; it moves translations into kingdom loads, untested on
  4 cores / the Deck). Next: the SPIR-V (919 MB in 17,065 files) into one compressed pack - zlib measured 3.4x.
- **#94's RAM on 32 GB PCs:** their logs show warm-up Long (32 GB and up: a budget of half the RAM, up to 12 GB)
  preparing 2,276-5,090 pipelines (30 s the first time) and area-shader preload running to its 8 GB budget: 8.6-10.2 GB
  in RAM. With the other port's cache (nothing our warm-up could use): 6.3 GB, but a 1,634 ms frame. Not changed yet.

- **RAM and cache, second round (all measured, local commits f95e78b, b94d9e3, 75153c7):** warm-up Auto 1 GB on every
  PC; only our tours' warm flags; SPIR-V in one zstd pack (old caches move into it); no pipeline binaries on NVIDIA.
  Totals, RTX 4090: a new player 98 s -> 51 s before the start, cache 2.2 GB -> 0.7 GB, RAM 8.4 -> 11.7 GB became
  4.6 -> 6.1 GB; this PC's own settings at 120 FPS through six kingdoms 14.7 -> 15.6 GB became 5.8 -> 6.6 GB, FPS and
  hitches the same. AMD iGPU: binaries make no RAM difference, kept. Not adopted: not loading our pipeline cache
  (another ~1 GB less, but NVIDIA's own cache holds 1 GB by default - too little for a play-through). Blocked: SMOL-V
  (a further ~2x on disk per its author) - downloading and building it was refused by the session's safety check.
  Next for RAM: what is left is ~3.2 GB of game memory and ~2.5 GB of driver objects; the step beyond is
  VK_EXT_shader_object (no pipeline objects at all), a renderer rewrite.

## Owner play card (after the automatic gates)
At most 10 minutes: enter the Lake Kingdom pond painting at 165 FPS; listen to the title music for a minute; play a
cutscene transition; turn FSR frame generation on and switch upscalers twice. The answer per item is fine / not fine.
