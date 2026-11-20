# 1.0.0 poison collision readback preview, revision 2

Issue [#3](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/3)
reports damage from invisible poison during the Wooded Kingdom Spewart fight
on game 1.0.0 / GTX 1060. The supplied save and log were used locally with a
freshly generated, compatible 1.0.0 game-code library. No report or save was
uploaded, and no production install or saves were changed.

## Evidence and candidate

`PaintObj::checkPaintAlpha` calls
`al::getColorFromLinearTexture(TextureData,x,y)` and tests the returned alpha's
high bit. The reader packs RGBA8 bytes into a color; its CPU memory pool was not
receiving the corresponding GPU render-target contents. However, this is not
the only CPU reader: `PaintObj::attackSensor` directly samples rows covering a
player's footprint. The first candidate missed that path and Simon confirmed
the issue persisted. Its successful single-texel comparisons were insufficient
to establish a fix.

The private Spewart run observed a native 128x128 RGBA8 (`0x25`) linear render
target, pitch 512. At (64,64), CPU memory contained `0x91890005` while the GPU
image contained `0x00000000`. Rendering into this exact texture was also logged;
this was not merely a comparison against an unrelated allocated image. Across
later queries CPU and GPU values continued to differ at clear mask samples.

Revision 2 also synchronizes the separate 372x372 RGBA8 damage mask (pitch 1536)
before `attackSensor` accesses its rows. Paint labeling consumes two additional
128x128 buffers and can edit them on the CPU. Both are synchronized before the
labeling loop; its inner per-pixel Map calls are left alone so they do not
overwrite edits in progress. The verified median-filter Map site uses the same
bounded snapshot helper, although that branch was not observed in these runs.

GPU data goes into private staging, then a bounded cache and a caller-owned
snapshot. The game thread copies rows into its requested allocation after
checking pitch, dimensions and pool bounds. The per-present GPU cache is reused
for footprint reads; labeling/filter snapshots explicitly refresh it after GPU
drawing within a present. Only native, single-layer, single-sample, GPU-owned
RGBA8 images with known linear storage qualify for bulk snapshots. Unknown
builds, unsupported formats and unregistered textures retain their behavior.
Blanket pass-end writeback remains off.

An explicit CPU flush over a fully synchronized paint texture then marks its
CPU contents for upload. This runs in queue order on the render thread, waits
for preceding work, invalidates the texture-view memo and paint readback cache,
and forces an upload at its next use, including render-target use. It does not
change ownership for arbitrary textures or vertex buffers.

The shared layout correction recognizes `LINEAR_RENDER_TARGET` (`0x40`) as
pitch-linear storage independently of `LINEAR` (`0x10`). Previously those rows
could be treated as GOB-swizzled data. Builder/object storage size, alignment,
implicit pitch, Vulkan row upload and CPU-upload checks now agree. Linear render
targets use 128-byte implicit row alignment, ordinary linear textures 32 bytes;
explicit pitches are preserved. A stride field alone does not make a texture
linear. These textures are excluded from scene-resolution scaling. This behavior
is also documented by the reference renderer's
[linear-target storage checks](https://github.com/seedds/nxnative/blob/main/docs/reference/nvn-objects.md).

Paint readback is confined to main build ID `3ca12dfaaf9c82da`. Verified
MemoryPoolMap return PCs and preserved inputs:

| Reader | Return PC | TextureData input |
| --- | --- | --- |
| Color texel | `0x7000a2b7cc` | x21; x/y in w20/w19, pool offset w22 |
| Damage footprint | `0x70002da5e0` | x21; pool offset w22 |
| Labeling entry | `0x70002d99a0` | actor x26, buffers at +0x79a8 / +0x7968 |
| Median filter | `0x70002dc49c` | x22; pool offset w19 |

Disassembly of the owner's 1.0.0 file verified these layouts and Map calls.
Pitch comes from TextureData+0x4c. The point hook preserves the reader's 32-bit
indexing. Existing F16 requests retain their two-byte interpretation.

Simon confirmed revision 2 completely fixed the issue. The verified 1.0.0 paint
paths are now **enabled by default**; `ODYSSEY_PAINT_CPU_READBACK=0` disables them
for comparison. The independent linear-layout correction
applies normally. `ODYSSEY_LOG_PAINT_READS=1` enables sampled CPU/GPU comparison
logs without itself changing CPU bytes.

## Checks and limits

- Windows build passed. `tools/test_linear_texture.c` exercises 0x10/0x40 flags,
  implicit/explicit pitches, nonuniform padded RGBA rows (including a case where
  the old GOB path changes pixels), round-trip row storage, untouched padding and
  rejected short/overflowing spans without partial writes.
- Bounded RTX 4090 runs at 3840x2160 exercised damage-mask snapshots, both label
  snapshots and CPU-edit flush/upload. With otherwise identical scripted input,
  the first candidate ended at 1 HP on clean ground; revision 2 ended at 3 HP.
  A separate forward-walk control entered visible poison and showed a purple
  damage reaction and 2 HP. Poison damage is preserved.
- The 1.4.1 Cap Kingdom regression also exited normally with valid nonzero F16
  samples. Its existing fog hook remains enabled and its copy size remains two
  bytes. The approved fog-preview executable was preserved.
- These controls support the specific clean-ground/damage correction; they do
  not establish every interaction in the fight. Two earlier attempted edge
  placements landed outside the arena and respawned; discard those as health
  comparisons.
- Manual checks must include crossing ground after clearing it with Cappy,
  taking damage from actual visible poison, and completing/retrying the fight.
  Simon confirmed the issue completely fixed in his manual test. The original
  reporter's GTX 1060 and other
  hardware, long sessions, experimental 120 FPS, other paint effects and Linux
  remain unverified. The Metal renderer does not implement this readback path.
- Only the verified reader/filter paths are covered. It is not a general NVN
  memory-coherence implementation or proof that every poison effect is fixed.

## Private reproduction

Artifacts are under `build/poison-review/` and must not be committed: reporter
save/log, game-derived placement JSON, settings, per-case copied saves, logs and
captures. The test helper `ODYSSEY_TEST_PLAYER_POS=2400:-4350:6800:-20400` moves
Mario near Spewart after frame 2400 on 1.0.0. It calls the game's
PlayerPuppet::resetPosition rather than changing simulation timing or leaving
stale custom collision state. It is inactive without the explicit variable.
`boss002` is the later flower-field entrance, not Spewart's arena.

`build/poison-layout/Test Poison Fix v2.cmd` enables the candidate, uses a copied reporter save and
private settings/cache, and allows normal controls. Resume, wait for the test
placement if necessary, advance/skip the Broodal introduction, and test the
fight. Restart the launcher to repeat the one-time placement after a death.

## Re-test recipe, 2026-11-18 (after the readback was made conditional, commit 2de3909)

The readback now happens only when the map changed (exec.c renderer_cpu_texel: `gpu_wgen`, `paint_cpu_epoch`).
How it was re-checked on game 1.0.0, scripted, about 2 minutes a case:

- Data: the 1.0.0 test copy (`ODYSSEY_DATA_ROOT`), the issue's save (`save_0.zip` on #3) copied into a scratch save
  folder. A released build run once on that copy builds its 1.0.0 game code; a dev exe with the same GAMELIB_ID then
  loads it.
- `ODYSSEY_TEST_PLAYER_POS=2400:-4350:6800:-20400` puts Mario at Spewart's arena; the Broodal introduction is skipped
  with `ODYSSEY_INPUT=...,2800:PLUS:8,2900:A:8,3000:PLUS:8,3100:A:8`. Mario then stands on clean ground at the
  arena's edge, the poison disc in front of him. Window shots (`ODYSSEY_HOST_INPUT=<frame>:shot:<name>`) show the
  life gauge.
- Cases and results (60 FPS, RTX 4090):
  | Case | Result |
  | --- | --- |
  | stand still, current code | 3 hearts throughout |
  | stand still, `ODYSSEY_PAINT_CPU_READBACK=0` (the #3 fix off) | 1 heart: damage on clean ground - the bug, so the test detects it |
  | `4200:STICK_UP:70` (walk into the poison), current code | 3 -> 2 hearts, purple damage reaction |
  | the same walk, `ODYSSEY_PAINT_SYNC_ALWAYS=1` (a readback every frame, as before) | the same: 3 -> 2 |
  | the walk with `ODYSSEY_PAINT_SYNC_VERIFY=1` | 4,266 left-out readbacks byte-equal to the kept copy, 0 different, 3,665 new copies in between |
- At 3840x2160 render and 120 FPS asked the fight ran at ~119 FPS either way; readbacks 236 -> 2 in 2 s.

Not covered: clearing poison with Cappy, finishing the fight, other hardware, Linux / Steam Deck.

## Clearing poison with Cappy, 2026-11-18 (the copy made where the map is written)

While Cappy clears poison the GPU writes the map between the game's reads, so each read needs the GPU's bytes; it
recorded a copy at the read and waited for everything queued. Now the map is copied out at the end of the submission
that wrote it (exec.c paint_copy_at_submit), and a read takes that copy, waiting for that submission alone.

- The owner's Wooded Kingdom recording with Cappy thrown every 1.5 s added at the poison (1.4.1, 4K render, 120 FPS
  asked, RTX 4090): 101-112 FPS while clearing before, 118-119 after; the waits ("paint CPU feedback probe", ~150 a
  second, ~0.75 ms each) are gone - none of the copies had to be waited for.
- `ODYSSEY_PAINT_SYNC_VERIFY=1` also makes the copy at the read and compares: 3,978 equal, 0 different in that
  replay; 2,040 and 271 equal, 0 different in the two 1.0.0 runs below.
- 1.0.0, Spewart's arena (the recipe above, plus `4200:Y:10` every 120 frames to 5000, then `5100:STICK_UP:70`):
  Cappy opens a path through the poison, Mario walks up it at 3 hearts; the same pictures and the same hearts as
  with `ODYSSEY_PAINT_COPY_AT_WRITE=0 ODYSSEY_PAINT_SYNC_ALWAYS=1`. Standing still: 3 hearts. Walking into poison:
  3 -> 2.

Not covered: the Steam Deck (a map last written on the compute queue keeps the copy at the read), other GPUs.

## Steam Deck, 2026-11-18 (the owner's second Wooded Kingdom recording replayed there)

Handheld mode, 60 FPS asked, `tools/linux/deck/deck-bench.sh` with `ODYSSEY_INPUT_REPLAY` (and `ODYSSEY_WARP=` in
GAME_ENV: the bench warps to Cap Kingdom otherwise). FPS in the 2 s windows of the ride:

| Build | FPS |
| --- | --- |
| a readback at every frame (0.4.6 and before) | 22-47, mostly 25-35 |
| only when the map changed + the copy where it is written | 28-61, mostly 35-55 |
| + no ask while nothing was queued since the last, + the map's pass in a submission of its own | 36-61, mostly 44-58 |
| the readback off (`ODYSSEY_PAINT_CPU_READBACK=0`: poison then hurts where none is shown) | 60 throughout |

- The copy made at the write waited ~5 ms for its submission ~40 times a second there (the frame's one long
  submission); with the map's pass ending its own submission (vk_paint_map_written -> split_if_pending) none waits.
- 91% of the asks are left out by the standing-queue check (objects.c paint_sync_rows), with no change in FPS: the
  round trips were not what cost the time.
- What is left: while Cappy clears poison the game's labeling pass asks with a refresh and hands CPU edits back,
  about four times a frame, and each waits for the render thread to finish the lists queued before it (~7 ms of
  work a frame on the Deck) - the game thread and the render thread then run one after the other. Below 60 FPS the
  game also runs extra ticks to keep its speed (ODYSSEY_GAMEPLAY_CATCHUP), which adds to it.
- `ODYSSEY_PAINT_SYNC_VERIFY=1` on the Deck: 2,873 copies at the write and 1,954 left-out readbacks equal, none
  different; on Windows also the left-out asks (0 wrong) and the 1.0.0 Spewart runs (Cappy's path, 3 -> 2 in poison).

## Steam Deck, 2026-11-19 (asks answered ahead of the queue and inside a list)

The same recording, Handheld mode, 60 FPS asked, 2 s windows of the ride (38 windows a run):

| Build | FPS |
| --- | --- |
| 0.4.8 / 0.4.9 | 36-61, median 52, mean 51.6; 18 windows under 50 |
| + asks run ahead of what is queued (`rq_ask`), between two lists | 32-61, median 55, mean 52.3; 16 under 50 |
| + answered inside the running list once nothing left in it writes a paint map | 38-61, median 60, mean 55.3; 8-11 under 50 |
| + the single texel reads (getColorFromLinearTexture) the same way | 45-61, median 60, mean 58.9-59.1; 1-3 under 50 |

- A profile of the game thread with call stacks (`ODYSSEY_SAMPLE_STACK=1`) showed where the time went: 27% of it in the
  wait of `rq_ask`, although no queued item was left to wait for - the render thread looked at a waiting ask only
  between two queued lists, and one list is most of a frame (2.3 ms waited an ask, four or five asks a frame).
- Inside a list (exec.c `paint_ask_poll`): the commands of the running list that write a paint map (it as a colour
  target, cleared, copied or downsampled into) are counted for the whole list and as they run; the ask is answered
  between two commands once none is left, no paint map is the colour target and no CPU edit waits for its upload.
  Only when nothing else queued before the ask draws or uploads, and not with the recording thread on (NVIDIA).
  `ODYSSEY_PAINT_MID=0`: between lists only. `ODYSSEY_PAINT_PRIO=0`: behind everything queued, as 0.4.9.
- Then 12% of the game thread was the single texel reads, each still waiting behind everything queued: they ask
  ahead too now. (Their first version counted the list again at every ask - hundreds a frame - and ran 14-61 FPS;
  the count is made once a list.)
- Checks on the Deck, none different: `ODYSSEY_PAINT_SYNC_VERIFY=1` - 76,000 row asks and 58,000 texel asks the same
  as the same ask behind everything queued (only ~2,000 of them inside a list: the check's own wait empties the
  queue); `ODYSSEY_PAINT_MID_VERIFY=1` (full speed) - 55,000 answers given inside a list the same when asked again
  at the end of that list. Cap Kingdom's benchmark walk: 59-61 FPS as before.
- What is left in the slowest stretch (frames ~1420-1650 of the ride, 45-50 FPS for 2-3 windows): the labeling
  pass's refresh asks that need a new copy from the GPU (~1-3 ms each) and asks that wait for the map's own passes
  later in the list - 14% of the game thread there.

- 1.0.0, Spewart's arena on Windows (the recipe above, RTX 4090): standing still 3 hearts; walking into the poison
  3 -> 2; Cappy thrown seven times opens the path at 3 hearts, and the run ends in the same picture and hearts as
  with `ODYSSEY_PAINT_PRIO=0`. `ODYSSEY_PAINT_SYNC_VERIFY=1`: 22,000 row asks and 70,000 texel asks the same, none
  different; `ODYSSEY_PAINT_MID_VERIFY=1` (recording thread off, so answers are given inside a list): 35,000 the same.

- The Deck through Steam in Gaming Mode (`deck-bench.sh LAUNCH=steam`, the 1.4.1 recording): 45-60 FPS, mean 59.1,
  1 of 39 windows under 50; gamescope's own screenshot shows the HUD in place.
- 1.0.0 on the Deck (a test copy in `/run/media/deck/SN01T/sopr-100`, game code built there in 897 s;
  `~/sopr/bench/sp100.sh`): standing still 3 hearts, walking into the poison 3 -> 2, Cappy's path open at 3 hearts.
  `ODYSSEY_PAINT_SYNC_VERIFY=1`: 112,000 texel asks the same, none different; `ODYSSEY_PAINT_MID_VERIFY=1`: 20,000
  answers inside a list the same.
- A trap in that test: later in the Cappy run the Deck's builds showed different hearts at the same frame (new 1,
  `ODYSSEY_PAINT_PRIO=0` 2, three runs each), while Windows gave one picture for every setting. It is the fight's
  timing, not the map: below 60 FPS the catch-up clock runs extra ticks, so Spewart's next attack falls on another
  frame. With `ODYSSEY_GAMEPLAY_CATCHUP=0` both give the same picture and hearts at each of six frames
  (5090-5470). Compare builds on the Deck with the catch-up clock off.

Not covered: a session played by hand on the Deck, other hardware.

