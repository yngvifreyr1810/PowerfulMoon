# Open bugs (2026-11-15)

Every open GitHub issue with its attached logs read, plus what came up in the owner's own testing. Status: **fixed**
(committed locally, not released), **cause known**, **needs repro**. Replies and closing issues happen at the end, by
the owner.

## Crashes

| # | Reports | What the logs show | Status |
|---|---------|--------------------|--------|
| **#40, #25, #19** | 0.2.10 RTX 4060; 0.2.8 RTX 2070 Super; 0.2.8 RX 6700 XT (Windows) | **120 FPS frame interpolation**: all three had it on. NVIDIA: `VK_ERROR_DEVICE_LOST` after the GPU wrote to an invalid address (`device fault: address write invalid 0` / `0x1d4fb000`); AMD: a crash inside amdvlk64.dll. #40: "crashes whenever I change a key bind ... only with 120 FPS interpolation on". | **Likely fix in** (in-betweens no longer replay draws that store to game memory: blended uniforms gave them garbage addresses). Not reproduced on the owner's PC (RTX, Cap Kingdom and the title screen, keyboard input): needs the reporters' retest. Interpolation is off by default; the option is now labelled experimental with a crash warning. |
| #25 (2nd report) | 0.2.8 RTX 2070 Super, windowed, interpolation off | Crash inside NVIDIA's driver (nvoglv64.dll), address 0x108. | **Needs repro** on a current build. |
| **#21** | 0.2.8 / 0.2.9, Linux, RX 7800 XT, **game 1.0.0** | Null pointer (0x98) in `al::ResourceSystem::createResource` (the game's resource loader), as the first level starts. romfs indexed 4636 files, nothing logged missing. | **Needs repro** with 1.0.0 on Linux (case-sensitive file names are a suspect). |
| Owner, Deck | 0.2.10+ dev, Gaming Mode | Crash report at every quit: a frame presented after the window was hidden; gamescope aborted ("surface already destroyed"). | **Fixed** (50ed7f8). |

## Steam Deck (owner's testing)

| Problem | Status |
|---------|--------|
| Resolution far below 1280x800: the game's own dynamic resolution (`al::GraphicsQualityController`, a nerve machine Wait -> Reduce -> Recover). | **Fixed**: setting **Dynamic resolution** (off by default on a Steam Deck) - the Reduce nerve's execute does nothing (1.0.0 0x7000a57f58, 1.4.1 0x70005ef6d0, from the RTTI). Deck, manual GPU clock 1000 MHz: docked stays 1600x900 at 53-60 FPS ~8.7 W, handheld 1280x720 at 58-60 FPS ~8.9 W. |
| 60 FPS at 7 W not reachable; target now **10 W** (+ manual GPU clock 1000 MHz recommended on the setup page). | In progress. |
| Steam keyboard popping up at launch / in the settings. | **Fixed** (6f264b6). |
| Stutters when new effects appear: game-thread stalls of 12-26 ms. | **Fixed**: a loading thread (priority 45) decoding SZS files held the game's main thread's core - its SVCs (file reads, locks) took the core back mid-decode. Decompression now runs off the core gate for its whole duration (`kernel_ungated_begin/_end`; `sead::SZSDecompressor` / `ParallelSZSDecompressor::tryDecompFromDevice`, both builds). Deck: gate waits in play 11-15 -> 0, first Cap Kingdom 59.4-60 FPS. Left: first-visit spikes when a new area's shaders build (one 64 ms frame in town). |
| Blocky stair-step edges on silhouettes against the sky. | **Needs comparison** with the Switch. |
| Motion controls: issues mentioned by the owner, not detailed yet. | **Needs details.** |
| #43 no sound on Steam Deck (0.2.10). The current build plays sound in Gaming Mode on the owner's Deck (recorded and measured); audio code unchanged since 0.2.10. | **Needs the reporter's log** (`[audren] device` line). |

## Performance

| # | Report | Status |
|---|--------|--------|
| #28 | Linux PCs drop 60 -> 5 FPS every few seconds (0.2.9 RX 9070 XT; 0.2.10 RTX 3060 Ti and a Steam Machine) | The 0.2.9 log matched #32 (fixed in 0.2.10). 0.2.10 reports remain: the Deck work since (compute queue, guarded writes, counter fix) may help - **needs a test on a Linux PC**. |
| #42 | Lake Kingdom: 27 FPS on sand (60 on grass); another player: a forest kingdom in slow motion | **Needs repro** (save / warp to Lake Kingdom). |
| #38 | Fossil Falls waterfall frame drops | **Needs repro**. |
| #13 | ROG Ally Z1 Extreme: slow motion at 30 FPS in corners | 0.2.10 changed present modes; the catch-up clock (game speed below 60) is Linux-only so far - Windows handhelds need it. |
| PC stutters on new effects | Owner's PC log (0.2.10): 31 frames held for shader builds in ~42,000. | **Fix in** (6f264b6: a short wait before holding a frame) - needs the owner's PC test. |

## Graphics

| # | Report | Status |
|---|--------|--------|
| #26 | Mario's shadow breaks into black squares in Cascade Kingdom (RX 6600 XT, 0.2.8) | **Needs repro** on a current build. |
| #25 | Rendering wrong with FSR 3.1 (0.2.8) | 0.2.10 fixed FSR 3.1 jitter (#24) - **retest**. |

## Input

| # | Report | Status |
|---|--------|--------|
| #40 | Key binds can't be changed on the launcher (configuration) page: the "press a key" prompt ignores mouse and keyboard. Mouse camera feels slow. | **Fixed** (the prompt only got presses while the in-game menu was open; now also on the configuration page) - needs the owner's test on the PC launcher. Mouse speed: a setting (25-400 %, default 100 %); default tuning goes with Phase 2's Windows controls. |
| #37 | Map flags / checkpoints can't be selected (PS4 controller, 1.4.1) | **Needs repro** (similar to closed #22, Switch controller). |
| #39, #27 | Map motion shake to a button | Feature - together with the owner's motion-control work. |

## Requests (not bugs)

#44 Korean in the F1 menu, #41 V-Sync option, #35 5.1 surround, #29 Brazilian Portuguese fan translation.

## Night of 2026-11-15 (local, not released; comments posted, nothing closed)

| # | Status |
|---|--------|
| #45 shader stutters after a full compile | Their 0.2.10 log: after a full preparation the game loaded **none** of the prepared libraries (994 absent at the first frame). The current build loads them: full preparation then play, 1,231 loaded / 2 absent on an RTX 4090 and on an AMD (Radeon iGPU) - commented, asked to retest the next build. Also: blended full-screen-sized draws no longer hold the picture; library builds ~45% cheaper on a Deck. |
| #41 V-Sync | **Done**: VSync On / Off / Strict / Adaptive, refresh rate (exclusive fullscreen), real monitor resolutions. Commented. |
| #39, #27 motion shake on a button | **Done**: Shake button (controller) / Shake key (middle mouse by default). Commented. |
| #13, #42 slow motion | **Fixed**: catch-up clock on by default on Windows (120 ticks / 2 s at ~32 FPS instead of 60). Commented; #42's frame drops themselves still need a log. |
| #46, #44 Korean | Commented (font has no Hangul / map font). Not fixed. |
| Owner: hard hitches at captures / boxes | **Fix in**: the picture is no longer held for a blended effect's first shader build. Needs the owner's play test. |
| Kingdom tour (PC, 17 kingdoms, cold caches) | All arrive, render and play; no crash. Holds only on each kingdom's first frame (pipelines building - an unprepared cache). |

## 2026-11-15 day (local; comments posted, nothing closed)

| # | Status |
|---|--------|
| #46 Korean map "???" | **Fixed**: shared system fonts by type (Korean / Chinese get Malgun Gothic, YaHei, JhengHei, Steam's Go Noto Kurrent on Linux / Deck). Commented. |
| #44 Korean F1 menu | **Done**: menu + launcher in Korean (runtime/ui/lang/ko.txt), Menu language setting. Checked on Windows and the Deck. Commented. |
| Deck shader preparation | Full preparation on a fresh folder: **5 min 52 s** (scan 58 s, translate 195 s, build 156 s); the first full one last week ~14 min. |
| Windows stage-entry / respawn pair | The stage's first frame is ~50 ms of GPU work: 1,331 render passes (the game's environment / probe setup, mostly 32x32 and 128x128 targets) + 526 feedback copies (8 ms). Game work, hidden by the fade on the Switch; the fix would be batching passes - open. |

## 2026-11-15 evening, after 0.2.11 (local commits; the owner allowed closing fixed issues)

Closed (fixed in 0.2.11, commented): #13, #27, #39, #44, #46; #50 (thanks, not an issue).

| # | Status |
|---|--------|
| #51 blocky picture when turning fast (Linux) | **Fixed** (b5bb266): the game's GPU timer counted the GPU's idle gaps between early-fence submissions, so its dynamic resolution dropped on a fast GPU. Busy time only now: PC, early fences on, camera spin: Reduce steps 10+ s -> none. Workaround commented (Dynamic resolution Off). |
| #53 gyro inversion / calibration | **Done** (a7ea5ee): Invert gyro X / Y, Calibrate gyro (saved offset). Asked which combination their controllers need. |
| Mip-chain feedback copies (every frame) | **Fixed** (e25a96b): blur / bloom chains rendering level N while sampling level N - 1 got a copy of level 0 each draw (~7 a frame, and the wrong level read). A view of levels 0..N-1 now: copies 840 -> 0 per 2 s, AMD iGPU 50.3 -> 53.2 FPS, same picture. |
| Stage-entry frame | Still ~56 + 69 ms on Windows: mostly CPU translation of the first frame (backend exec ~40 ms for 3,081 draws), then GPU. The copies above were not its cause. Open. |
| #40 keybind crash with interpolation | Launcher binding fixed in 0.2.11. The in-game crash (device lost on NVIDIA, 0.2.10) did not reproduce on 0.2.11 (4 rebinds with 120 FPS interpolation). Asked to retest. |
| #37 map flags | Works on 0.2.11 (controller: cursor on a flag, "Warp here", A warps). Asked for details. |
| #19 AMD interpolation crash | Not reproduced on the PC's AMD iGPU (Cap, interpolation on). Open. |
| #21 1.0.0 crash in LayoutSystem::initFont (Linux) | Needs 1.0.0 game files (not on the PC). Open, owner's. |
| #41 tearing with every VSync option, #43 no sound on the Deck, #54 video only, #42 / #38 slow spots | Asked for logs / details. |
| #52 mouse camera feels like a stick | The game's camera eases mouse input like a stick; a direct-angle camera means hooking the camera poser. Asked what feels off. Open. |
| #35 5.1, #48 amiibo, #29 PT-BR | Feature requests, acknowledged; open. |

## 2026-11-15 night: features (local commits)

| Feature | Status |
|---|--------|
| #35 5.1 surround | **Done** (a1f3c6e): Audio output Auto / Stereo / 5.1. The game mixes its own 5.1 when told the TV has 6 channels; centre, LFE and rears carry sound (PC, levels logged). Deck: Auto = stereo. |
| Frame generation | **Done, Windows** (1a6da80): AMD FSR frame generation (SDK 2.4.0 Vulkan swapchain), with DLSS / FSR / XeSS upscaling. RTX 4090: 60 -> 114-120 presented; AMD iGPU 40 -> 80; swapchain remakes and off / on during play fine. Not yet: a HUD-less image (the HUD may shimmer on generated frames). DLSS-FG (Streamline, RTX 40+) and XeSS-FG (DX12 only) not done. |
| #52 mouse camera | **Done** (f4ec2cb): the follow camera turns exactly as far as the mouse moves (no stick easing), vertical within -35..85. Other camera kinds still take the mouse as a stick. Only tested on 1.4.1 (1.0.0 offsets from its disassembly). |
| FSR 2 for Deck / Linux | Scoped, not started: dlss_probe.c (scene detection, jitter, depth, motion vectors) is Windows-only and needs to be made portable first, then FSR 2.2's passes ported (GLSL shaders + ~1,200 lines of host code). |

## 2026-11-15 night 2: the owner's list

| Item | Status |
|---|--------|
| FSR 2 on Linux / Deck | **Done** (213a714): FSR 2.2.1 passes on our own host code; Deck docked 16:9 at Match display works (853x480 -> 1280x720, 60 FPS). Not yet with the Deck's defaults (Handheld + 16:10 fill: the input isn't scaled down). |
| PR #49 (PT-BR) | Reviewed (no network / process / file-deletion code; hashes the 4 mod files), merged onto the current code in a worktree, tested Windows + Deck (English and PT-BR-without-package: no faults). Commented; merge is the owner's call. |
| DLSS frame generation + Reflex | **Wired, beta** (08168d7): Streamline 2.14.16 manual hooking; Reflex on; DLSS-G status OK and stable, but in scripted (unfocused) runs DLSS-G reports 1 frame presented per rendered frame - needs a check on a real display with FrameView / the overlay. PresentMon (ETW) was blocked in this session. XeSS FG / XeLL: DirectX 12 only - not possible on Vulkan. |
| Shadows depending on the camera | **Fixed** (depth clamp): the game turns on depth clamp (nvnCommandBufferSetDepthClamp) for its sun shadow map, which the runtime ignored. Casters between the sun and the map's near plane (the Odyssey's balloon over Mushroom Kingdom's plaza, the fountain, trees) were then clipped away - the more the camera looked down, the closer the fitted near plane and the more vanished. Now recorded and applied (Vulkan depthClampEnable, in the vertex library's state; library key bit 3, ODYSSEY_DEPTH_CLAMP=0 for the old behaviour). Checked against Eden at the fountain: level, tilted up, looking down - same shadows; 120 FPS unchanged. |
| HUD sharper / ultrawide HUD at the edges | **Done** (a098dd8, 7cf7244): the game's layout draws (nn::ui2d::Layout::Draw, marked) go to a layer at the window's resolution while DLSS / FSR / XeSS or a lower internal resolution is in use, composited after the upscale - at DLSS Performance 1440p the HUD is as sharp as native. Video > Screen shape > HUD position (wide): Edges (default) / Centered (16:9). Tested 21:9 / 32:9, native and DLSS: gameplay, pause menu, map, cutscene. Not yet on the Deck (it was off). |
| Native frame rate up to 240 | **Done, as far as the game allows** (b98a75f): Frame rate 60 / 72 / 90 / 100 / 120 / 144 / 165 / 180 / 240; every frame really drawn, Mario and the camera at that frame's moment (no generated images). Gameplay logic stays at the game's 60 steps a second: its code counts frames (timers, physics, animations), so running it faster would speed the game up. RTX 4090 1280x720: 72 / 144 / 240 FPS held, 60 ticks a second, camera pan even (5-6 px every frame at 240). |
| FSR 2 at the Deck's defaults | **Done, docked** (b1f0402): 100% and any picture shape (16:10 Fill) now let the mode lower the input (853x533 -> 1280x800). Handheld mode not yet: its scene is the 1280x720 corner of the window texture. |
| PR #56 (pt-BR installer) | Reviewed: no malicious code found (HTTPS only, size limit, SHA-256 pinned before extraction, system tar without a shell, fingerprints after, no overwrite; Linux bsdtar built from libarchive's pinned source). Windows: builds, offline tests 7 passed / 6 skipped (need the archive / network). Not run on the Deck (off). Open question for the owner: automated download of a third-party translation from its Google Drive link (no redistribution / automation grant found). |

## 2026-11-16: owner's 0.3.0 test findings

| Item | Status |
|---|---|
| Shader stutter after updating | **Fixed** (1057abb): a cache marked prepared was never prepared again - after a driver update (saved pipeline binaries refused) or a new version every effect built while playing. The mark now records GPU, driver and app version; a change makes the preparation due. Owner's protocol (cache deleted, NVIDIA's disk cache redirected to an empty folder, full preparation, play): 0 shaders built in Cap Kingdom, 0 held frames. |
| Preparation time | 31 threads instead of 16 (155f976): cold 136.7 s -> 110.0 s on an RTX 4090. |
| Start time | NGX's DLSS check on its own thread (cd6a043): first frames 1.5 s sooner. Left: 1.3 s of single-threaded pipeline warm-up before the game. |
| Controller screen still shown | **Fixed** (a8ccb5e): with Skip on it is not drawn at all - black until the title. |
| Shadows vanishing (Sand Kingdom ruins, camera looking down / two steps) | Same cause as the row above (depth clamp ignored), **fixed with it**; checked in Mushroom Kingdom against Eden, not yet at the Sand ruins. |
