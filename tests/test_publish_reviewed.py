"""Promotion must use the exact audited artifact, not a newer rebuild."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/automation'))
from publish_reviewed import verified_files

class Promotion(unittest.TestCase):
    def fixture(self, folder):
        version = '0.2.1-powerfulmoon.1'; sha = 'a' * 40
        (folder / 'release.json').write_text(json.dumps({'version': version}))
        payload = folder / ('PowerfulMoon-' + version + '-windows-x64.zip')
        with zipfile.ZipFile(payload, 'w') as archive:
            archive.writestr('PowerfulMoon/PACKAGE.json', json.dumps({'name': 'PowerfulMoon', 'version': version, 'source_commit': sha}))
        setup = folder / ('PowerfulMoon-Setup-' + version + '.exe'); setup.write_bytes(b'synthetic setup')
        for path in (payload, setup):
            path.with_suffix(path.suffix + '.sha256').write_text(hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + path.name)
        audit = {'source_commit': sha, 'forbidden_paths_found': [], 'embedded_setup_payload_matches_zip': True,
                 'game_files_keys_saves_generated_game_library_included': False,
                 'zip_sha256': hashlib.sha256(payload.read_bytes()).hexdigest(), 'setup_sha256': hashlib.sha256(setup.read_bytes()).hexdigest()}
        (folder / 'PowerfulMoon-content-audit.json').write_text(json.dumps(audit))
        return sha, setup
    def test_only_exact_audited_commit_can_promote(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp); sha, setup = self.fixture(folder)
            meta, files = verified_files(folder, sha)
            self.assertEqual(len(files), 5)
            with self.assertRaises(ValueError): verified_files(folder, 'b' * 40)
            setup.write_bytes(b'tampered')
            with self.assertRaises(ValueError): verified_files(folder, sha)
    def test_additional_payload_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp); sha, _ = self.fixture(folder)
            (folder / 'unexpected.exe').write_bytes(b'not audited')
            with self.assertRaises(ValueError): verified_files(folder, sha)

if __name__ == '__main__': unittest.main()
