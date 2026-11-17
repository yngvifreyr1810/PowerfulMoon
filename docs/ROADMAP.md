# Roadmap (owner's goals, 2026-11-15)

The order work happens in. Details, measurements and rules: [AI-HANDOFF.md](AI-HANDOFF.md).

## Phase 1 - Steam Deck (now, top priority)

Target: **a real 1280x800 picture, a locked 60 FPS with a flat frame-time graph, at a low power limit (7 W tested),
1:1 with the Switch, gyro working out of the box.** Tested in Gaming Mode through the dev shortcut (play-dev.sh).

1. **Real 1280x800.** Today the game's own dynamic resolution drops to its minimum (960x540 of 1600x900 = 768x480 on
   the Deck) within seconds of entering Cap Kingdom at 7 W, and never comes back - that is the "looks far lower than
   1280x800" image; SMAA cannot fix it.
   - Found (1.0.0 symbols): `agl::lyr::RenderDisplay` keeps a requested frame-buffer size at +0x9f0 with a
     "changed" flag at +0xa48; `RenderDisplay::calc` applies it through `resetFrameBufferSize_` every frame. The
     code that WRITES the request (the dynamic-resolution decision) uses a computed pointer: next step is a runtime
     write-watch on that field (runtime/dispatch.c watch facility) to name it, then a generator patch
     (tools/odyssey_patches.py) behind a setting **"Dynamic resolution: Switch / Off (always full)"**, Off on the Deck.
   - **Done 2026-11-15:** found as `al::GraphicsQualityController` (its Reduce nerve, hooked at runtime from the RTTI in
     both builds); setting "Dynamic resolution", off on the Deck. With the manual GPU clock at 1000 MHz: 60 FPS at
     ~9 W in both console modes at full resolution.
   - Then measure 60 FPS at full 1280x800 at 7 W. At 7 W the Deck's GPU is about a docked Switch's: GPU time per
     frame has to come down (next item) - or the power limit rises a little (owner's call), or a lower internal
     resolution with a good upscaler (Phase 3).
2. **GPU time per frame** (profile: ODYSSEY_GPUPROF_AT): ~23 passes a frame that only clear (fold into the next
   pass), ~72 one-draw passes' load/store cost, barriers. Done so far: depth read in place, compute queue.
3. **Frame pacing like RivaTuner's limiter.** A precise frame limiter at the present (fixed 16.67 ms cadence, sleep +
   short spin, the presenter thread), FIFO / mailbox / immediate choices, and frame-time stats (p99, max, a
   histogram) in the log and overlay, so "flat 100% of the time" is measured, not eyeballed. Then the remaining
   single hitches: first-use shader compiles that still block, texture uploads at stage changes, game-thread stalls.
   - **Done 2026-11-15 (Linux):** present timing (runtime/nvn/vulkan/pacing.c, VK_KHR_present_wait) logs what the
     display really showed every 10 s (`[display]`: shown / dropped / held, spacing p50 / p99 / max), and the overlay's
     graph plots it. The 60 Hz timer is locked to the display (vi.c, ODYSSEY_VSYNC_LOCK, offset 9 ms): images held a
     second refresh in a 50 s Deck walk went from 5-9 to 0-2, each left one a game hitch. Present to screen is ~47 ms,
     all of it after our GPU work (gamescope). Next: the game hitches themselves; measure the lock on Windows.
   - **Done 2026-11-15:** the fog map's first CPU read no longer waits for the GPU (exec.c renderer_cpu_texel,
     objects.c): a ~60 ms hitch two frames into Cap Kingdom gone. Left on the bench route: the stage's first frame
     (~95 ms of first uses spread over views, texture hashing, uploads, allocations - no single cause), which the
     display shows as one or two images held. Render-thread profiles: SAMPLE_AT + GAME_ENV=ODYSSEY_SAMPLE_THREAD_NAME=
     render,ODYSSEY_SAMPLE_STACK=1, then tools/linux/deck/stacks.py on the Deck.
   - **Effects route (2026-11-15):** tools/linux/deck/effects-route.txt (throws, spin throw, jumps, ground pounds,
     rolls, dives, camera, two falls, pause menu, map) with `GAME_ENV=ODYSSEY_WARP=` - with the owner's save the
     default warp at 1200 starts the Odyssey's travel cutscene, so the plain bench's "walk" was that cutscene.
     `SHOTS=<frame>,...` saves the window at those frames to check where a route went (it stalls the presenter:
     don't measure in the same run). 85 s of play: 9 images held. Shaders for new effects (44-228 ms builds) all
     built in the background - no frame waited, none was skipped. The held ones: the stage-entry autosave moment
     (~frame 1435, 45 ms, game thread waiting on a guest worker) and respawns after a fall (~50 ms, the stage-entry
     first-use pattern). Not found: which guest worker the game thread waits for at ~1435.
   - **Owner's hard hitches (2026-11-15, Deck): breaking boxes and captures, ~180 ms.** Their session log: runs of
     frames "not shown (a full-screen pass is building)" - 17, 10, 5, 2 frames - each as long as one vertex library's
     first build (100-313 ms). Only 23 of 2,041 libraries in that session weren't saved binaries: effects new to the
     install, which the 20-minute preparation doesn't cover (it builds the known list). Changed: a full-screen-sized
     draw that is blended over the scene or scissored (a sprite close to the camera) no longer holds the picture at
     any frame rate (was 120 FPS only; ODYSSEY_HOLD_OVERLAYS=1 restores); held draws now log their target and blend.
     Not verified on captures / boxes yet (the scripted routes don't reach them).
   - **Vertex libraries are slow to build** (cold run, 605 of them: 175 ms CPU each on average, fragment 60 ms): our
     vertex SPIR-V is ~6x the fragment's (median 183 KB vs 31 KB; 1,245 as_f / as_u helper calls in a median one,
     570 NoContraction decorations from `precise gl_Position`). Shrinking it (macros instead of helper functions)
     would change every module's key - every player's saved shaders rebuilt - so measure the gain first.
     Tools: DATA=/run/media/deck/SN01T/sopr-cold (game files linked, its own caches) for cold runs; library lines in
     the shader-time log now show their CPU time.
4. **Gyro on the Deck, automatic.** The Switch's motion controls (shaking to throw Cappy, motion aiming) from the
   Deck's gyro through SDL3 sensors - working when the game is added to Steam without a Steam Input setup (to be
   tested in Gaming Mode: Steam Input owns the controller there) - and a settings page for gyro (sensitivity,
   invert, shake threshold, on/off).
5. **Everything for 1.0.0 too** (tools/port141.py maps patch sites between versions) **and Windows** (the Linux-only
   pieces - early fences, guard, compute queue - are measured there before they're turned on).

## Phase 2 - Windows

- **GPU selection** in the settings: each GPU listed by its name (e.g. "NVIDIA GeForce RTX 4090"), the dedicated one
  chosen by default - never shown as "Default".
- **Keyboard and mouse defaults that make sense** (left mouse: throw Cappy, Space: jump, WASD: move, mouse: camera,
  the rest following common PC platformer layouts), **motion controls with the mouse** (hold e.g. Ctrl and move),
  and a settings page for motion controls.

## Phase 3 - Image quality at low power

- **Plan (2026-11-15):** Windows has DLSS, FSR 3.1, FSR 4 (beta) and XeSS through the vendors' prebuilt Windows
  runtimes. The Deck / Linux have FSR 1 only. A temporal upscaler there needs either AMD's FidelityFX SDK built for
  Linux (its shader permutations come from a Windows-only tool) or a port of FSR 2's open shaders to the renderer's
  GLSL (a few thousand lines; the jitter and motion vectors the Windows path feeds DLSS are reused). The port is the
  realistic route; it needs the FSR 2 source downloaded (MIT). Frame generation: FSR 3 frame generation is
  Windows/DX12-centric; on Linux the practical options are the existing 120 FPS interpolation or a driver-level one.
- **Done 2026-11-15:** FSR frame generation on Windows (AMD's SDK 2.4.0 made it Vulkan): runtime/nvn/vulkan/fsr3_sdk.c,
  with any of the upscalers. Next for it: a HUD-less back buffer (the scene before the HUD, scaled as the present pass
  does) so the HUD stays still on generated frames.

- A better upscaler for low internal resolutions. HelixSR / "FSR4-lite" is D3D12; options on Vulkan: AMD FSR 3.1
  (third_party/fsr3_sdk, Vulkan), Intel XeSS (Vulkan), or porting a shader-based one - license checked first.

## Phase 4 - Sharing

- A public GitHub repository with the optimizations for others - only when the owner says so, under the rules in
  AI-HANDOFF.md (nothing published without the owner's go-ahead).
