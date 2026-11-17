"""Real C settings/mod-loader tests. No game assets are distributed with the tests.

Build tools/test_localization.c with settings.c, mods.c, localization.c,
nxcrypto.c and SDL3; pass the executable as the first argument.
Optional: PTBR_TEST_ROMFS points to the separately obtained official v1.1 romfs.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

FIXTURE = str(Path(sys.argv.pop(1)).resolve())
PACKAGE = os.environ.get('PTBR_TEST_ROMFS')
FILES = ('LayoutData/FontData.szs', 'MessageData/LayoutMessage.szs',
         'MessageData/StageMessage.szs', 'MessageData/SystemMessage.szs')
MISSING, READY, DISABLED, INCOMPLETE, CONFLICT, CODE = range(6)


class LocalizationTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.ini = self.root / 'settings.ini'
        self.ini.write_text('language = pt-BR\n', encoding='utf-8')

    def run_settings(self):
        env = dict(os.environ, ODYSSEY_SETTINGS=str(self.ini))
        result = subprocess.run([FIXTURE], cwd=self.root, env=env, capture_output=True,
                                text=True, encoding='utf-8', timeout=15, check=True)
        return json.loads(result.stdout)

    def install(self, name='Translation', layout='romfs'):
        if not PACKAGE:
            self.skipTest('PTBR_TEST_ROMFS not supplied; private translation assets required')
        dest = self.root / 'mods' / name / layout
        shutil.copytree(PACKAGE, dest)
        return dest

    def test_missing_translation_retains_preference_across_restart(self):
        first = self.run_settings()
        self.assertEqual(first['status'], MISSING)
        self.assertEqual(first['code'], 'en-US')
        self.assertEqual(first['language'], 12)
        self.assertEqual(first['name'], 'Português (Brasil)')
        self.assertEqual(first, self.run_settings())
        self.assertIn('language = pt-BR', self.ini.read_text())

    def test_original_languages_and_invalid_value_round_trip(self):
        for code in ('auto', 'ja', 'en-US', 'es', 'fr', 'de', 'it', 'nl', 'ru', 'ko', 'zh-CN', 'zh-TW', 'invalid'):
            with self.subTest(code=code):
                self.ini.write_text(f'language = {code}\n')
                first = self.run_settings()
                self.assertEqual(first, self.run_settings())
                expected = 'auto' if code == 'invalid' else code
                self.assertIn(f'language = {expected}\n', self.ini.read_text())

    def test_game_and_menu_language_preferences_are_independent(self):
        for menu in ('auto', 'en', 'ko'):
            for game in ('pt-BR', 'ja', 'en-US'):
                with self.subTest(menu=menu, game=game):
                    self.ini.write_text(f'language = {game}\nmenu_language = {menu}\n')
                    first = self.run_settings()
                    self.assertEqual(first, self.run_settings())
                    self.assertIn(f'language = {game}\n', self.ini.read_text())
                    self.assertIn(f'menu_language = {menu}\n', self.ini.read_text())
                    self.assertEqual(first['code'], 'en-US' if game == 'pt-BR' else game)

    def test_unrelated_mod_and_fake_english_files_are_not_translation(self):
        root = self.root / 'mods/JumpManClub Brasil/romfs/LocalizedData/USen'
        for file in FILES:
            path = root / file
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'Unrelated synthetic test data')
        result = self.run_settings()
        self.assertEqual(result['status'], MISSING)
        self.assertEqual(result['files'], 4)

    def test_other_title_id_is_ignored(self):
        path = self.root / 'mods/Other/atmosphere/contents/0100000000000001/romfs/example'
        path.parent.mkdir(parents=True)
        path.write_bytes(b'Synthetic')
        self.assertEqual(self.run_settings()['mods'], 0)

    def test_disabled_unrelated_mod_survives_save(self):
        path = self.root / 'mods/Other/romfs/example'
        path.parent.mkdir(parents=True)
        path.write_bytes(b'Synthetic')
        self.ini.write_text('language = pt-BR\nmod_off = Other/romfs\nvolume = 0.42\n')
        result = self.run_settings()
        self.assertEqual(result['files'], 0)
        self.assertEqual(result, self.run_settings())
        self.assertIn('mod_off = Other/romfs', self.ini.read_text())
        self.assertIn('volume = 0.42', self.ini.read_text())

    def test_official_package_and_atmosphere_layout(self):
        self.install(layout='atmosphere/contents/0100000000010000/romfs')
        first = self.run_settings()
        self.assertEqual(first['status'], READY)
        self.assertEqual(first['code'], 'en-US')
        self.assertEqual(first['files'], 4)
        self.assertEqual(first, self.run_settings())

    def test_missing_font_is_incomplete(self):
        dest = self.install()
        (dest / 'LocalizedData/USen/LayoutData/FontData.szs').unlink()
        self.assertEqual(self.run_settings()['status'], INCOMPLETE)

    def test_modified_and_truncated_assets_are_rejected(self):
        dest = self.install()
        path = dest / 'LocalizedData/USen/MessageData/StageMessage.szs'
        original = path.read_bytes()
        for data in (original[:-1], original + b'x', bytes([original[0] ^ 1]) + original[1:]):
            with self.subTest(size=len(data)):
                path.write_bytes(data)
                self.assertEqual(self.run_settings()['status'], INCOMPLETE)

    def test_disabled_translation_and_reenable(self):
        self.install()
        self.ini.write_text('language = pt-BR\nmod_off = Translation/romfs\n')
        first = self.run_settings()
        self.assertEqual(first['status'], DISABLED)
        self.assertEqual(first, self.run_settings())
        self.ini.write_text('language = pt-BR\n')
        self.assertEqual(self.run_settings()['status'], READY)

    def test_higher_priority_conflict_and_lower_priority_nonconflict(self):
        self.install()
        path = self.root / 'mods/A-conflict/romfs/LocalizedData/USen/MessageData/StageMessage.szs'
        path.parent.mkdir(parents=True)
        path.write_bytes(b'Synthetic conflicting replacement')
        self.assertEqual(self.run_settings()['status'], CONFLICT)
        (self.root / 'mods/A-conflict').rename(self.root / 'mods/Z-conflict')
        self.assertEqual(self.run_settings()['status'], READY)

    def test_code_patches_report_unsupported(self):
        dest = self.install()
        path = dest.parent / 'exefs/patch.ips'
        path.parent.mkdir()
        path.write_bytes(b'Synthetic unsupported code patch')
        self.assertEqual(self.run_settings()['status'], CODE)

    def test_case_insensitive_romfs_paths(self):
        dest = self.install()
        for folder in ('LocalizedData/USen/MessageData', 'LocalizedData/USen/LayoutData'):
            for path in (dest / folder).iterdir():
                path.rename(path.with_name('temporary-name'))
                path.with_name('temporary-name').rename(path.with_name(path.name.upper()))
        self.assertEqual(self.run_settings()['status'], READY)


if __name__ == '__main__':
    unittest.main()
