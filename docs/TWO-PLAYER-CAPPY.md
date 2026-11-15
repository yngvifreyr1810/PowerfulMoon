# Two-player Cappy controllers

Connect two controllers. Use the controller you want for Mario first, open
F1 > Controls, and choose the other controller under **Cappy controller** in
the bindings section. Close settings, then open the game's pause menu and choose
**Two Players**. The assignment is local to this running session.

Mario keeps the active controller and keyboard/mouse. The selected second
controller reports as FullKey player two, with its own buttons, sticks, short
presses and rumble. Bindings and the stick deadzone apply to both controllers.
The second controller cannot take over Mario by pressing a button. The PC
settings menu still uses its usual controls and blocks both players' game input.

If the second controller disconnects, it stops reporting connected immediately.
A unique matching device reconnects automatically without taking Mario's pad.
Serial numbers are used when present, otherwise its SDL device path and GUID;
ambiguous matches require selecting the controller again. Choose **Not assigned**
to cancel the assignment and automatic reconnection. Use the game's Single Player
option before removing the second player permanently.

Secondary motion controls are not implemented: Cappy receives a stationary
six-axis sample, never Mario's gyro/accelerometer. Normal Cappy movement/buttons
work. Independent gyro-camera settings remain a separate task.

## Validation

`tools/test_input_players.c` links the actual secondary input implementation
against SDL's process-local virtual controllers. It checks separate inputs,
remapping, radial deadzones, nn::hid stick-direction bits, menu capture,
press/release between gameplay polls, capture during a tick, rumble, disconnect,
unique reconnect, ambiguous reconnect and explicit cancellation. It passed on
Windows and Linux (the latter inside the private Deck build container).

A 1.4.1 gameplay run with experimental 120 FPS selected entered the actual
**Two Players** menu. ControllerSupport requested 2..2 players and received two.
The game consumed player-two stick/button packets while Mario moved separately.
The second device was disconnected and reattached with a new SDL instance ID;
Mario kept the original device. The frame-6100 capture shows the game's two-player
controls and Cappy in the world. This run exited zero.

Subsequent runs on 1.0.0 at 60 FPS and 1.4.1 with 120 FPS selected both reached
frame 6100 and exited zero. Both logged fresh player-two stick/button input after
reconnection under a new SDL instance ID, with the primary ID unchanged. Both
captures were viewed and show the game's co-op controls and Cappy separately.
These checks include the event-loop starvation fix described in the main test
record. A final guard also keeps Mario reserved during Cappy's disconnected
interval; the SDL fixture verifies that reservation until explicit cancellation.

The initial virtual-device results do not establish physical rumble strength
or platform-specific reconnect identity.
Simon subsequently tested a DualSense for Mario and a Switch Pro for Cappy:
movement mostly worked, but Cappy's right-stick camera and mixed-family guide
icons needed the follow-up below.
The final controller selector was captured at 1280x720 and checked for readable
labels and alignment with the surrounding settings rows; that run exited zero.

ControllerSupport argument/result offsets were checked against the
[libnx definitions](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/applets/hid_la.h).
The applet response honors the requested maximum and single-player mode flag;
assigning Cappy does not turn a single-player boot prompt into co-op.

For private regression runs only, `ODYSSEY_CAPPY_TEST=1` creates two virtual
controllers inside the process, assigns the second, exercises it at frames
4000-4300, disconnects/reconnects at 5500/5700 and exercises it again at 5800-6000.
`ODYSSEY_CAPPY_TRACE=1` logs player-two HID transitions. Neither switch is needed
for normal play. No virtual devices are registered outside the game process.

## Mixed-controller guide and shared camera

The co-op camera's original arbitration treated Mario's stick as active above
0.001, then discarded the chosen camera vector below 0.03. Drift between those
thresholds blocked Cappy without moving the camera. A single call-site patch
uses the existing 0.03 threshold when a Cappy controller is assigned. The game's
remaining arbitration still gives intentional Mario input priority and keeps
its capture restrictions. Neither movement nor global vector math is changed.
The equivalent call site was checked in both 1.0.0 and 1.4.1.

Mario and Cappy now have separate icon cells within the existing controller-font
sheet. Only Cappy's two tutorial panes select the additional cells; recognized
FullKey icon tags resolve directly to those cells, while other formatting tags
and Mario's guide retain normal processing. The device family is read separately
for each assignment. No HUD layout or game assets are changed. The font reader
also follows the selected asset root instead of assuming the current directory.

`tools/test_original_wrapper.py` verifies that selective generated wrappers
intercept direct and indirect calls and retain a callable original body.
`tools/test_coop_prompts.c` checks mixed-family atlas cells, switching families,
regular-font fallback, Cappy-only pane scope, embedded-zero formatting tags and
controller-icon tags containing multiple layout alternatives. These and the
secondary input fixture passed on Windows. Current follow-up changes have not
yet been retested with physical controllers or on Linux.

The camera's 1.0.0 positive/negative control runs (4900 frames, exit zero) logged
50 horizontal and 25 vertical Cappy camera samples with the fix, versus zero
with it disabled. Both retained deliberate Mario input (50/51 samples). A
1.4.1 run with 120 selected also exercised both Cappy axes and Mario priority,
and exited zero. Camera captures were viewed. The final 1.4.1 mixed-guide
capture at frame 3500 shows PlayStation Cross for Mario's jump and Nintendo Y
for Cappy's flight, with both stick icons intact; that run exited zero.
The final 1.0.0 run also reached frame 3500, exited zero and produced the same
correct mixed-family guide in the viewed capture.

For the camera regression, `ODYSSEY_CAPPY_CAMERA_TEST=1` adds 0.01526 drift to
Mario's right stick, then exercises Cappy's horizontal and vertical camera input
and simultaneous deliberate input from Mario. `ODYSSEY_COOP_CAMERA_FIX_OFF=1`
retains the stock comparison for a negative control. Both are private test
switches, used with `ODYSSEY_CAPPY_TEST=1`, and are unnecessary for normal play.
