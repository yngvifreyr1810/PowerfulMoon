# Work and validation after 0.2.9

Requested by Simon: work through all remaining items, keeping changes separately
reviewable and testing them properly. Base source: `1ac024d` (tag `v0.2.9`).
Private builds, save copies and captures stay under ignored `build/029-tests/`.
No production saves/install edits, pushes or release publication in this work.
Windows tests use logical processors 16-31 on the current 32-thread PC.

## Completed before this work

The old PRIORITY-TEST-QUEUE predates Claude's 0.2.9 work. Clean Windows dependency
packaging and Windows/Linux GitHub Actions builds are implemented; successful
builds and both release assets were checked. Crash reports, display selection,
the expanded overlay and fog readback are present. Poison readback now covers
1.4.1 as well as 1.0.0. Language, mouse, audio and Linux-font fixes shipped too.
An open issue can still require reporter confirmation after its fix ships.

## Ordered acceptance checks

| Item | Implementation / required evidence |
| --- | --- |
| 1.4.1 loading at 90/120 FPS | Deterministically reproduce `TextureDataEx::reset` before changing behavior. Test stock 60, normal 90/120, strict diagnostic extra draws, repeated stage changes, new-game opening and a matching 1.0.0 library. Verify requested cadence resumes after loading and gameplay remains 60 Hz. |
| Shadow distortion (#26) | Reporter case is RX 6600 XT / Cascade / 0.2.8. Obtain settings and 0.2.9 retest; local NVIDIA captures cannot establish an AMD fix. Test native, FSR and DLSS independently at 60 and experimental rates. |
| FSR jitter and distant bloom (#24) | Identify the exact FSR backend. Compare static and moving-camera sequences with native and DLAA controls, including lamps, hair, fences, particles and UI. Preserve each backend's sharpness/CAS state. |
| Full kingdom tour | All 17 actual destinations, adequate settling, fresh per-run save copies, logs and captures; compare with the same game version/save/settings/renderer inputs. Manually review differences. Separate fog/poison/combat/interior/cutscene tests remain required. |
| Graphics controls | Shadow resolution, draw/LOD/effect distances: stock defaults and bounded independent options; validate render correctness, culling, memory and frame time on both game builds. Do not claim that raising a render target alone increases shadow detail. |
| Ultrawide | 21:9 and 32:9 gameplay, HUD, menus, map, cutscene bars, transitions and restoring 16:9. A wide-window screenshot alone is insufficient. |
| VRR and display cap | Presentation cap independent of simulation; validate monitor selection, 60/90/120, 144 Hz output, fullscreen modes and VRR hardware pacing. A 144 Hz presentation cap does not imply 144 Hz game logic. |
| Deck controls | Importable Steam Input configuration, gyro and appropriate prompts; actual hardware, docking and controller reconnection. |
| Deck sleep/resume | Mid-game suspend/resume, audio, gyro/input, device/swapchain restoration, timing and saves. Requires the Deck. |
| Add to Steam | Explicit opt-in, correct quoted AppImage/data paths, duplicate detection, closed-Steam handling and rollback; fixtures plus real Steam verification. |
| FSR Lite | Establish a compatible Vulkan ML provider/model and actual Deck support before exposing a setting. Preserve analytical FSR fallback; do not label it ML. |
| Camera controls | Independent horizontal/vertical sensitivity and inversion for stick, mouse and gyro. Preserve legacy settings and avoid changing gyro shake detection/capture controls. |
| Two-player Cappy | Explicit player-two selection, player-one retention, correct HID IDs, disconnect/reconnect and game co-op flow; requires two-controller testing. |
| Vendor frame generation | Actual supported NVIDIA/AMD/Intel implementations, UI composition, pacing and latency; distinguish vendor generation from existing game interpolation. GPU-specific tests required. |
| Sharper HUD/menu | Preserve stock layout, clipping, font fallback, shadows and post-processing on both game versions; test native/temporal upscalers and 1080p/1440p/4K. |

## Current loading investigation

Normal timing controls reached/loaded Wooded at 120 FPS and exited normally.
Strict alternating extra draws reproduced the known 1.4.1 fault twice on the
first Wooded draw: access `0x20` in `0x70009917d0` (`TextureDataEx::reset`).
Preparing graphics on gameplay frames as well as extra frames did not fix it;
that candidate was discarded.

Keeping asynchronous scene setup and its initial warm-up at stock 60 Hz, then
restoring the requested experimental cadence, passed the strict reproduction.
A normal 90 FPS test also loaded and captured Wooded. Further checks are in
progress; this is a scene-loading safeguard, not a general allocator rewrite.
`ODYSSEY_NATIVE_120_SAFE_LOAD=0` disables it for diagnostic negative controls.
It applies only to 1.4.1; stock 60 and the 1.0.0 draw path are unchanged.

The final default-enabled strict 120 test reached frame 3000 and exited zero.
The normal 90 run captured frame 3800; a normal 120 run captured frame 6500.
The latter logged effective cadence 60 followed by 120 and approximately 120
gameplay/controller ticks per two seconds during play. These are bounded
behavior checks, not CPU performance measurements (private compilation also
ran on the test processors). A matching current 1.0.0 library was rebuilt from
the owner's files without bypassing the library ID; its 120 FPS Cap test reached
frame 3300 and exited zero. Artifacts have descriptive run names in
`build/029-tests/`. The final repeated-transition run reached 10000 frames,
Cap -> Wooded -> Cap, including reuse of the StageScene address. Effective
cadence was exactly 60,120,60,120,60,120: one loading hold per scene, no in-play
flutter. The shader-preload heuristic is deliberately separate from this cadence
hold; a delayed control call must not switch play back to 60. A second strict
Wooded reproduction with this refinement reached frame 3000 and exited zero.
A fresh empty-save opening with 120 selected reached frame 6500 and exited zero;
its capture shows Cappy after the Bowser encounter. The safeguard correctly held
this scene at stock 60. Live frame-rate switching still needs a check.

The published 0.2.9 Windows ZIP was downloaded for the renderer baseline and
verified against its release digest:
`393882cebd9a51d40c58b2a7d3dcc979d06f95f4a819799d2aeb517d84fae20a`.
Only runtime files were extracted for the private tour; its completed-save
source is copied separately for every run.

The tour runner now supports 90 FPS, explicit capture dimensions, processor
affinity and strict diagnostics. Timeout cleanup must actually terminate its
synthetic parent/child tree promptly; waiting for natural exit is not a pass.
The four comparator/cleanup checks passed in 0.432 s with process permissions.

Only observed or tester-confirmed checks may be marked passed. Unavailable
hardware, missing reproductions and pending tests remain explicit open items.

## Additional validation

- Both published 0.2.9 and candidate tours captured all 17 actual destinations,
  each from a fresh save copy, and exited zero. Nine comparisons met thresholds;
  eight required review. All comparison sheets were viewed: differences were
  concentrated in animated fog, water, effects and characters, without obvious
  missing geometry or HUD. This does not cover every interaction or effect.
- Independent stick/mouse X/Y settings passed four parser/persistence tests and
  three bounded real-game input runs, including legacy mouse settings. A menu
  capture was checked for readability. Separate gyro axes remain unimplemented.
- Cap gameplay at 2560x1080 and gameplay/settings at 3840x1080 exited zero; the
  captures show correct proportions, a centered HUD safe area and readable menu.
  Maps, cutscene bars and restoring 16:9 remain open.
- Simon manually confirmed the private Deck AppImage's Add to Steam, launch from
  the new Steam entry, controls, audio and mid-game sleep/resume all passed.
  That preview uses default 60 FPS and predates the final cadence refinement.
  This does not establish handheld performance, a custom Steam Input layout,
  gyro-axis settings, reconnect behavior or an FSR ML provider on the Deck.
  The shortcut helper's eight fixture tests also pass on Windows and native
  Deck Python, including quoted paths, preserving existing entries, duplicate
  detection, backup/undo checks and multi-account selection.
- A private Linux gameplay run exited zero and its capture was viewed. Busy Cap
  performance was about 34 FPS; a separate menu scene was about 20 FPS. These
  are observations, not an optimization or a matched release comparison.

## FSR Native AA reproduction

Issue #24's clip uses FSR 3.1 Native AA, fixed 4K internal resolution, and a
1080p output. A matching private run exited zero but logged viewport jitter
without a single FSR evaluation: the temporal eligibility check rejected the
unequal sizes. Native AA must resolve at the internal size, then let present
scale the result to the window. The same correction applies to DLAA and XeSS
Native AA. Their post-sharpening passes use the resolved image's dimensions;
the ordinary fallback retains the display dimensions.

The first corrected FSR run reached frame 3900 and logged at least 2640 resolved
frames at 3840x2160. DLAA and XeSS runs both reached frame 3300 with at least
1920 resolved frames each, at the same internal size. All three exited zero;
history traces show no depth-image churn or frame gaps, and no repeated history
resets after settling. All three final captures were viewed. The camera turn
was exercised, but still captures alone cannot prove that all distant-bloom
flicker is resolved. Final-build FSR Native AA (3840x2160 resolve) and Quality
(1280x720 -> 1920x1080) controls both reached frame 3000, exited zero and logged
at least 1680 evaluations each. Their histories settled and stayed continuous
through the camera turn. The Quality capture was also viewed; reconstruction
of moving objects and HUD quality are separate outstanding work.

Unsupported fixed-resolution upscaling combinations now use the same size
eligibility rule for raster jitter and temporal evaluation. An old-build Quality
test with 4K input / 1080p output had 15 nonzero jitter samples and zero resolves;
the guarded run had 15 zero-jitter samples and zero resolves. Both exited zero.
This is an ordinary-upscaler fallback, not FSR operating in a downscaling mode.
A supported fixed 1080p input / 4K output Quality run reached frame 3000 with
at least 1680 FSR evaluations. Jitter sequence length now uses the actual scale
instead of the quality preset's nominal scale. A Native AA control also exited
zero at frame 3000. Moving characters still lack object motion vectors.

## Controller/UI event starvation

The 1.0.0 two-player fixture exposed starvation in the host event loop: waiting
up to 16 ms after every event never reaches periodic work while a controller
delivers sensor samples more frequently. Window requests and controller setup
can remain pending. The loop now waits once and drains at most 128 queued events
before returning to maintenance, on Vulkan and Metal.

The real SDL event-flood regression test passed on Windows and Linux. In the
Windows check, the old loop allowed one maintenance opportunity with a 196.2 ms
gap; the new loop allowed 82 with a longest 16.2 ms gap. Linux measured 194.6 ms
versus 16.1 ms. The test also verifies the 128-event bound with a queued flood.
After this fix, the formerly starved 1.0.0 gameplay fixture initializes both
virtual pads and enters the actual Two Players menu. Mac compilation/hardware
testing remains unavailable; only its identical loop replacement is reviewed.

## Two-player Cappy preview

A session-local controller selector is implemented in Controls, in the bindings
section. Separate FullKey player-two input, deadzone/remapping, native short-press
latching, menu blocking, controller-change events and rumble routing are present.
The primary controller remains reserved while Cappy disconnects/reconnects.
Secondary motion reports a stationary sample; it is not implemented.

The actual input module passed process-local SDL virtual-controller checks on
Windows and Linux. Private gameplay runs on 1.0.0 (60 FPS) and 1.4.1 (120 selected)
both entered the real Two Players menu, exercised simultaneous independent
inputs, disconnected/reattached the secondary device, consumed fresh secondary
input after reconnect, captured frame 6100 and exited zero. Both captures were
viewed. Physical input and rumble remain pending: Simon confirmed he can test
on PC. See TWO-PLAYER-CAPPY.md for scope and the regression procedure.

Simon tested DualSense/Mario and Switch Pro/Cappy. Movement mostly worked;
he reported that Cappy's right stick could not move the camera and both guides
used Mario's controller family. The follow-up aligns the co-op arbitration's
drift threshold with its downstream camera cutoff and supplies separate font
cells for Cappy's guide. Both game versions' call sites were checked.

The 1.0.0 camera positive/negative control demonstrated Cappy's horizontal and
vertical input only with the fix; intentional Mario input retained priority in
both runs. A 1.4.1 run with 120 selected exercised the same camera branches.
Final mixed-guide runs at 60 FPS on 1.0.0 and 1.4.1 exited zero at frame 3500;
viewed captures show PlayStation Cross for Mario and Nintendo Y for Cappy.
Focused Windows input, mixed-atlas/tag/font-fallback, and generated-wrapper
tests passed. The follow-up still needs Simon's physical-controller retest;
Linux was not rerun for these changes. No installation, production save,
remote branch or release was changed.
