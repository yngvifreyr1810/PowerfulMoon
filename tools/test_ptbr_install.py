"""Actual installer/filesystem/extractor checks; private assets are optional.

Pass the native fixture executable. PTBR_TEST_ARCHIVE enables the official
RAR cases; PTBR_TEST_NETWORK=1 additionally opts into one real official download.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

FIXTURE = str(Path(sys.argv.pop(1)).resolve())
ARCHIVE = os.environ.get('PTBR_TEST_ARCHIVE')
READY, FAILED, CANCELLED = 5, 6, 7


class InstallerTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / 'data & games'
        self.root.mkdir()
        (self.root / 'save').mkdir()
        (self.root / 'save/progress.bin').write_bytes(b'Synthetic save fixture')
        (self.root / 'powermoon.ini').write_text('language = ja\nmod_off = Other/romfs\n')
        (self.root / 'mods/Other/romfs').mkdir(parents=True)
        (self.root / 'mods/Other/romfs/file.txt').write_bytes(b'Unrelated mod')
        self.before = self.snapshot()

    def snapshot(self):
        return {str(p.relative_to(self.root)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in self.root.rglob('*') if p.is_file()}

    def run_install(self, archive, mode='local'):
        result = subprocess.run([FIXTURE, mode, str(self.root), str(archive)],
                                capture_output=True, text=True, encoding='utf-8', timeout=100, check=True)
        value = json.loads(result.stdout)
        for path, digest in self.before.items():
            self.assertEqual(hashlib.sha256((self.root / path).read_bytes()).hexdigest(), digest)
        self.assertEqual(list((self.root / 'mods').glob('.ptbr-install-*')), [])
        return value

    def package(self):
        if not ARCHIVE:
            self.skipTest('PTBR_TEST_ARCHIVE not supplied; official package is private test data')
        return Path(ARCHIVE).resolve()

    def test_missing_download_file_preserves_existing_data(self):
        value = self.run_install(self.root / 'missing.rar')
        self.assertEqual(value['state'], FAILED)
        self.assertEqual(self.snapshot(), self.before)

    def test_invalid_or_html_download_is_rejected(self):
        bad = self.root.parent / 'package.rar'
        for content in (b'', b'<html>Sign in</html>', b'Rar!\x1a\x07\x01\x00' + b'x' * 100):
            with self.subTest(size=len(content)):
                bad.write_bytes(content)
                value = self.run_install(bad)
                self.assertEqual(value['state'], FAILED)
                self.assertEqual(self.snapshot(), self.before)

    def test_oversized_archive_is_rejected(self):
        bad = self.root.parent / 'huge.rar'
        bad.write_bytes(b'x' * 800000)
        self.assertEqual(self.run_install(bad)['state'], FAILED)
        self.assertEqual(self.snapshot(), self.before)

    def test_same_size_unknown_archive_fails_before_extraction(self):
        bad = self.root.parent / 'unknown.rar'
        bad.write_bytes(b'x' * 773795)
        value = self.run_install(bad)
        self.assertEqual(value['state'], FAILED)
        self.assertIn('checksum', value['error'])
        self.assertEqual(self.snapshot(), self.before)

    def test_existing_destination_is_not_overwritten(self):
        target = self.root / 'mods/JumpManClub Brasil'
        target.mkdir()
        (target / 'keep.txt').write_bytes(b'Existing user content')
        before = self.snapshot()
        value = self.run_install(self.root / 'missing.rar')
        self.assertEqual(value['state'], FAILED)
        self.assertIn('already exists', value['error'])
        self.assertEqual(self.snapshot(), before)

    def test_even_empty_existing_destination_is_preserved(self):
        target = self.root / 'mods/JumpManClub Brasil'
        target.mkdir()
        self.assertEqual(self.run_install(self.root / 'missing.rar')['state'], FAILED)
        self.assertTrue(target.is_dir())

    def test_cancel_does_not_publish_any_mod(self):
        value = self.run_install(self.root / 'missing.rar', 'cancel')
        self.assertEqual(value['state'], CANCELLED)
        self.assertEqual(self.snapshot(), self.before)

    def test_symlinked_mods_directory_is_refused(self):
        outside = self.root.parent / 'outside'
        outside.mkdir()
        empty_root = self.root.parent / 'linked data'
        empty_root.mkdir()
        try:
            (empty_root / 'mods').symlink_to(outside, target_is_directory=True)
        except OSError:
            self.skipTest('Symlink creation unavailable on this host')
        result = subprocess.run([FIXTURE, 'local', str(empty_root), 'missing.rar'],
                                capture_output=True, text=True, encoding='utf-8', timeout=15, check=True)
        self.assertEqual(json.loads(result.stdout)['state'], FAILED)
        self.assertEqual(list(outside.iterdir()), [])

    def assert_package_installed(self):
        mod = self.root / 'mods/JumpManClub Brasil/romfs/LocalizedData/USen'
        files = sorted(p.relative_to(mod).as_posix() for p in mod.rglob('*') if p.is_file())
        self.assertEqual(files, ['LayoutData/FontData.szs', 'MessageData/LayoutMessage.szs',
                                 'MessageData/StageMessage.szs', 'MessageData/SystemMessage.szs'])

    def test_official_archive_installs_all_four_files(self):
        value = self.run_install(self.package())
        self.assertEqual(value['state'], READY, value['error'])
        self.assertEqual(value['progress'], 1)
        self.assert_package_installed()

    def test_official_archive_in_path_with_spaces_and_shell_characters(self):
        source = self.package()
        copy = self.root.parent / 'official & translation.rar'
        shutil.copyfile(source, copy)
        value = self.run_install(copy)
        self.assertEqual(value['state'], READY, value['error'])
        self.assert_package_installed()

    def test_official_archive_with_unicode_paths(self):
        source = self.package()
        root = self.root.parent / 'instala\u00e7\u00e3o & tradu\u00e7\u00e3o'
        self.root.rename(root)
        self.root = root
        archive = root.parent / 'tradu\u00e7\u00e3o.rar'
        shutil.copyfile(source, archive)
        value = self.run_install(archive)
        self.assertEqual(value['state'], READY, value['error'])
        self.assert_package_installed()

    def test_modified_official_archive_is_rejected(self):
        data = bytearray(self.package().read_bytes())
        data[-1] ^= 1
        modified = self.root.parent / 'modified.rar'
        modified.write_bytes(data)
        self.assertEqual(self.run_install(modified)['state'], FAILED)
        self.assertEqual(self.snapshot(), self.before)

    @unittest.skipUnless(os.environ.get('PTBR_TEST_NETWORK') == '1', 'Real network test is opt-in')
    def test_download_from_the_official_link(self):
        value = self.run_install('-', 'download')
        self.assertEqual(value['state'], READY, value['error'])
        self.assert_package_installed()


if __name__ == '__main__':
    unittest.main()
