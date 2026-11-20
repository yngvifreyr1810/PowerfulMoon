# Plan: heavy views (New Donk City power lines) - hold the frame rate, use less CPU

2026-11-17. Owner's report: riding the power lines in New Donk City stutters at 120 FPS and also drops below 60 at
the 60 FPS setting (RTX 4090, game 1.4.1, 0.4.3). A drop to 60 as a fallback was tried and rejected (it felt worse).

## What is known (measured)

- The view draws ~2,300-2,500 objects a frame, about four times a normal view. The GPU is idle (~3 ms a frame).
- A slow frame there: ~14 ms in the game's own code (recompiled ARM64), then ~9 ms translating its command lists
  (our backend). They can't overlap: the game waits for each frame's translation (`nvnSyncWait` on the frame's fence),
  because the translation reads game memory the game rewrites for the next frame.
- At 120 FPS the game also runs its whole draw pass a second time per logic tick (the extra frame).
- Already done (local commits 0b7c2ae, 24a43ed): backend 5.1 -> 4.2-4.5 ms a frame in Metro's heavy views (batched
  recording queue, descriptor sets reused, two wasted scans removed). Game code at -O2: +1% FPS, not felt.
- Not reproduced by script yet: my Metro script stays at 117 FPS; only the owner's ride shows the bursts.

## Step 1 - record once, replay forever (so every fix is measured on the real ride)

1. **Input recorder** in the port: `ODYSSEY_INPUT_RECORD=<file>` writes what the game is given at each input sample
   (buttons, both sticks, motion) with the game's frame number; `ODYSSEY_INPUT_REPLAY=<file>` feeds it back instead
   of the controller. Both hook the one place the game reads its pad from (`runtime/hle/input.c`, where scripted
   `ODYSSEY_INPUT` already goes in).
2. **A launcher for the owner** ("Record the power line.bat"): starts the test copy with the owner's save at 60 FPS,
   records inputs and Mario's position each second, and logs every frame over 17 ms. The owner rides the line down
   and up twice and quits. Done once.
3. **Replay check**: the same file replayed three times must put Mario on the same positions (the position trace)
   within a small tolerance. Loads take a different number of frames each run, so replay is keyed to the game's own
   frame counter from the moment the stage is running, not to wall time. If the replay drifts anyway, fallback: place
   Mario on the recorded positions with the existing `ODYSSEY_PLACE` tool and replay only the camera.
4. **The gate**, printed by one script (`powerline.sh`): at 60 FPS - frames over 17 ms and the lowest 2 s FPS; at
   120 FPS - frames over 12 / 16 / 20 ms; per frame the game's time, the backend's time and the draw count; CPU use of
   the process. Baseline first (3 runs each), then every change against it.

## Step 2 - find where the game's 14 ms goes (not guessed)

5. Sample all threads during the replay (`ODYSSEY_SAMPLE`), and resolve the game's functions by name using game
   1.0.0 (it has symbols; same ride recorded there too). Questions: which game functions dominate; how much is
   our HLE layer under them (`hle_tramp`, `rt_dispatch`, command recording: `nvnCommandBuffer*` calls are HLE, one per
   command, ~2,400 x N a frame); how much is waiting.
6. Same for the render thread on the ride (it has different draws than my Metro script).

## Step 3 - fixes, in order of expected gain per risk (each behind a switch, each measured on the replay)

A. **HLE call overhead for command recording** - if step 5 shows it: every NVN command the game records crosses from
   recompiled code into our C through a trampoline. Make the hot ones (bind, draw, set uniform buffer) direct calls
   and thin. Expected: a real part of the game-side 14 ms; low risk.
B. **Backend per-draw cost** (continues today's work): state keys hashed incrementally instead of per draw, uniform
   copies deduplicated across passes (`guest_copy` is 3% + memcpy), pipeline lookup by pointer, the command-list walk
   (`exec_list` 10% self time). Target: 4.3 -> ~3 ms a frame in heavy views.
C. **Skip redundant draw passes at 120**: the extra frame re-runs passes whose result can't change between the two
   draws of one logic tick with the same light (shadow maps, some depth pre-passes). Reuse them. Expected: the largest
   single win at 120 FPS; medium risk (picture errors if a pass does depend on the camera) - checked by screenshots.
D. **Game code quality**: hot game functions found in step 5 compiled with better settings, or the lifter's patterns
   for them improved (flags, memory access). Only for functions the profile names.
E. **Experiment, last: snapshot-and-continue** - copy what a frame's translation reads at the fence so the game can
   go on. Only if A-D leave a gap; days of work, earlier partial attempt was slower.

Not planned: a 60 FPS fallback (rejected by the owner; the code stays off, `ODYSSEY_FRAME_BRAKE=1`).

## Done means

On the recorded ride, three runs each: at 60 FPS no frame over 17 ms in play; at 120 FPS no burst of frames over
16 ms (single frames over 12 ms allowed); lower CPU use than the baseline; screenshots along the ride unchanged; the
kingdom routes and the Steam Deck bench no worse.
