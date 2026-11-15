"""Shortcut transactions against temporary Steam-like folders; never the real client."""
from pathlib import Path
import shlex
import struct
import tempfile
import unittest
from unittest.mock import patch

import steam_shortcut as steam


class ShortcutTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.config = self.root / 'userdata/123/config'
        self.config.mkdir(parents=True)
        self.path = self.config / 'shortcuts.vdf'
        self.game = self.root / "Simon's game folder"
        self.game.mkdir()
        self.image = self.game / 'Super Odyssey.AppImage'
        self.image.touch()
        self.receipt = self.game / 'undo.json'
        self.running = patch.object(steam, 'steam_running', return_value=False)
        self.running.start()
        self.addCleanup(self.running.stop)

    def add(self):
        return steam.add(self.root, self.image, self.game, self.receipt)

    def test_known_binary_layout_round_trips_without_changes(self):
        # A literal client-format object with one appid/string entry, not output
        # produced by the encoder under test.
        literal = (b'\0shortcuts\0\x000\0\x02appid\0\x78\x56\x34\x92'
                   b'\x01AppName\0Other game\0\x00tags\0\x010\0Favorite\0\x08\x08\x08')
        entries, ending = steam.decode(literal)
        self.assertEqual(entries[0][2][0][2], struct.pack('<I', 0x92345678))
        self.assertEqual(steam.encode(entries, ending), literal)

    def test_add_deduplicates_and_quotes_paths_with_spaces_and_apostrophes(self):
        self.add()
        first = self.path.read_bytes()
        self.assertIn('Already', self.add())
        self.assertEqual(self.path.read_bytes(), first)
        entries, _ = steam.decode(first)
        self.assertEqual(len(entries), 1)
        fields = {k: v for _, k, v in entries[0][2]}
        self.assertEqual(shlex.split(fields['Exe']), [str(self.image.resolve())])
        command = fields['LaunchOptions'].replace('%command%', shlex.quote(str(self.image.resolve())))
        self.assertEqual(shlex.split(command), ['env', 'ODYSSEY_DATA_ROOT=' + str(self.game.resolve()), str(self.image.resolve())])
        self.assertTrue(struct.unpack('<I', fields['appid'])[0] & 0x80000000)
        steam.undo(self.receipt)
        self.assertFalse(self.path.exists())

    def test_other_entries_unknown_fields_and_undo_are_preserved(self):
        original = steam.encode([(0, '4', [(1, 'AppName', 'Unrelated'), (7, 'FutureCounter', b'12345678'),
                                          (0, 'tags', [(1, '0', 'Favorite')])])], ending=b'')
        self.path.write_bytes(original)
        self.add()
        entries, ending = steam.decode(self.path.read_bytes())
        self.assertEqual(entries[0], steam.decode(original)[0][0])
        self.assertEqual(ending, b'')
        steam.undo(self.receipt)
        self.assertEqual(self.path.read_bytes(), original)

    def test_moving_appimage_preserves_appid_and_custom_fields(self):
        self.add()
        entries, ending = steam.decode(self.path.read_bytes())
        appid = entries[0][2][0]
        entries[0][2].append((1, 'CustomFutureField', 'keep me'))
        self.path.write_bytes(steam.encode(entries, ending))
        self.image = self.game / 'Updated build.AppImage'
        self.image.touch()
        self.add()
        updated, _ = steam.decode(self.path.read_bytes())
        self.assertEqual(len(updated), 1)
        self.assertEqual(updated[0][2][0], appid)
        self.assertIn((1, 'CustomFutureField', 'keep me'), updated[0][2])

    def test_undo_does_not_clobber_newer_steam_changes(self):
        self.add()
        changed, ending = steam.decode(self.path.read_bytes())
        changed.append((0, '99', [(1, 'AppName', 'A newer game')]))
        newer = steam.encode(changed, ending)
        self.path.write_bytes(newer)
        with self.assertRaises(ValueError):
            steam.undo(self.receipt)
        self.assertEqual(self.path.read_bytes(), newer)

    def test_running_steam_and_corrupt_input_leave_original_untouched(self):
        for corrupt in (b'\0shortcuts\0', b'not a VDF', b'\0shortcuts\0\x05broken\0'):
            self.path.write_bytes(corrupt)
            with self.assertRaises(ValueError):
                self.add()
            self.assertEqual(self.path.read_bytes(), corrupt)
        self.path.unlink()
        with patch.object(steam, 'steam_running', return_value=True):
            with self.assertRaises(ValueError):
                self.add()
        self.assertFalse(self.path.exists())

    def test_account_selection_uses_most_recent_or_refuses_ambiguity(self):
        (self.root / 'userdata/456/config').mkdir(parents=True)
        with self.assertRaises(ValueError):
            self.add()
        (self.root / 'config').mkdir()
        (self.root / 'config/loginusers.vdf').write_text('"users" { "76561197960266184" { "MostRecent" "1" } }')
        self.assertEqual(steam.account(self.root), '456')
        self.assertEqual(steam.account(self.root, '123'), '123')

    def test_login_persona_braces_and_quotes_do_not_confuse_account_selection(self):
        (self.root / 'userdata/456/config').mkdir(parents=True)
        (self.root / 'config').mkdir()
        (self.root / 'config/loginusers.vdf').write_text(
            '// client metadata\n"users" { "76561197960266184" { '
            '"PersonaName" "Player {name} \\"quoted\\"" "MostRecent" "1" } }')
        self.assertEqual(steam.account(self.root), '456')


if __name__ == '__main__':
    unittest.main()
