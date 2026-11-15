"""Legacy camera settings and independent-axis persistence through the real C parser.

Build tools/test_camera_settings.c with runtime/settings.c and SDL3, then pass
the executable path as the first argument. Each case uses a new process/file.
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

FIXTURE = str(Path(sys.argv.pop(1)).resolve())


class CameraSettingsTests(unittest.TestCase):
    def run_settings(self, content):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'test.ini'
            if content is not None:
                path.write_text(content)
            def read():
                return json.loads(subprocess.check_output([FIXTURE, str(path)], text=True, timeout=10))
            first = read()
            self.assertEqual(first, read(), 'A saved configuration must survive a new process')
            return first

    def test_old_combined_mouse_speed_initializes_both_axes(self):
        value = self.run_settings('mouse_sensitivity = 300\nmouse_invert_y = 1\n')
        self.assertEqual(value['mouse'], [300, 300, 0, 1])
        self.assertEqual(value['stick'], [100, 100, 0, 0])
        self.assertEqual(value['motion'], 1)

    def test_new_axes_are_independent_in_either_key_order(self):
        keys = ['mouse_sensitivity = 200', 'mouse_sensitivity_y = 50', 'mouse_invert_x = 1',
                'stick_camera_x = 25', 'stick_camera_y = 400', 'stick_camera_invert_y = 1']
        for order in (keys, keys[::-1]):
            with self.subTest(order=order):
                value = self.run_settings('\n'.join(order))
                self.assertEqual(value['mouse'], [200, 50, 1, 0])
                self.assertEqual(value['stick'], [25, 400, 0, 1])
                self.assertEqual(value['motion'], 1)

    def test_invalid_ranges_are_bounded(self):
        value = self.run_settings('mouse_sensitivity = 999\nmouse_sensitivity_y = -4\n'
                                  'stick_camera_x = 0\nstick_camera_y = 1000\n')
        self.assertEqual(value['mouse'][:2], [400, 25])
        self.assertEqual(value['stick'][:2], [25, 400])

    def test_new_install_keeps_original_controls(self):
        value = self.run_settings(None)
        self.assertEqual(value['mouse'], [100, 100, 0, 0])
        self.assertEqual(value['stick'], [100, 100, 0, 0])


if __name__ == '__main__':
    unittest.main()
