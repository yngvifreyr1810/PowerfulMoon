# Independent stick and mouse camera axes

Controls now has horizontal/vertical speed (25-400%) and inversion for the stick
and mouse separately. Defaults retain the original input behavior. Stick changes
apply at ProjectCameraInput's main-gameplay-camera boundary, rather than changing
HID stick values: Mario's movement and menu navigation are not rescaled.
The game's own camera inversion settings still apply after these controls.

The legacy `mouse_sensitivity` key remains horizontal speed. An older file with
only that key initializes both axes to its saved value. New files also save
`mouse_sensitivity_y`; it takes precedence for vertical speed regardless of key
order. Existing `mouse_invert_y` remains supported, with `mouse_invert_x` added.
The stick uses `stick_camera_x/y` and `stick_camera_invert_x/y`.

This does not implement independent gyro-camera sensitivity. Scaling the whole
six-axis sample would also alter Cappy's shake detection and other motion moves,
so that requires a separate camera-specific integration.

## Validation

- Windows executable and Linux executable built successfully.
- Four real C settings-parser tests passed: legacy combined sensitivity, both
  new-key orders, bounded values, defaults and save/new-process persistence.
- Three 1.4.1 private gameplay runs exited normally and captured frame 3300.
  Each exercised 20 logged horizontal-stick samples, 20 vertical samples and
  20 mouse samples through the actual game camera callback. Defaults were
  unchanged; the independent case produced stick factors -0.25/-2 and mouse
  offsets -0.6/+0.15, while the old combined 200% setting produced +1.2/-1.2.
- The Controls menu was captured at 1280x720 and inspected; independent mouse
  rows and inversion toggles are visible without changing the original HUD.
- Logs/settings/screenshots stay in ignored `build/029-tests/camera-*`.

Special camera modes and physical controller feel still need manual testing.
