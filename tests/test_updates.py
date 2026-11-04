"""Network-free updater checks: approved channel, ordering, origin and integrity."""
import hashlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/windows'))
from update_client import API, REPO, SafeRedirect, check, download, parse_release
from version_info import version_tuple

def release(version='0.2.1-powerfulmoon.1', payload=b'installer'):
    name = 'PowerfulMoon-Setup-' + version + '.exe'
    return {'tag_name': 'v' + version, 'draft': False, 'prerelease': False, 'published_at': '2026-11-05',
            'assets': [{'name': name, 'browser_download_url': 'https://github.com/' + REPO + '/releases/download/v' + version + '/' + name,
                        'digest': 'sha256:' + hashlib.sha256(payload).hexdigest(), 'size': len(payload)}]}

class Updates(unittest.TestCase):
    def test_numeric_order_and_same_version_patch(self):
        self.assertGreater(version_tuple('0.2.10-powerfulmoon.1'), version_tuple('0.2.9-powerfulmoon.7'))
        self.assertIsNotNone(parse_release(release('0.2.0-powerfulmoon.3'), '0.2.0-powerfulmoon.2'))
        self.assertIsNone(parse_release(release(), '0.2.1-powerfulmoon.1'))
        self.assertIsNone(parse_release(release(), '0.3.0-powerfulmoon.1'))
    def test_candidates_and_upstream_are_never_updates(self):
        for key in ('draft', 'prerelease'):
            data = release(); data[key] = True
            self.assertIsNone(parse_release(data, '0.2.0-powerfulmoon.2'))
        data = release(); data['tag_name'] = 'v0.2.1'
        with self.assertRaises(ValueError): parse_release(data, '0.2.0-powerfulmoon.2')
    def test_rejects_wrong_origin_missing_digest_or_duplicate_asset(self):
        data = release(); data['assets'][0]['browser_download_url'] = 'https://evil.invalid/setup.exe'
        with self.assertRaises(ValueError): parse_release(data, '0.2.0-powerfulmoon.2')
        data = release(); data['assets'][0]['digest'] = ''
        with self.assertRaises(ValueError): parse_release(data, '0.2.0-powerfulmoon.2')
        data = release(); data['assets'] *= 2
        with self.assertRaises(ValueError): parse_release(data, '0.2.0-powerfulmoon.2')
    def test_download_verifies_bytes_and_removes_partial(self):
        update = parse_release(release(), '0.2.0-powerfulmoon.2')
        with tempfile.TemporaryDirectory() as folder:
            target = download(update, folder, opener=lambda _: io.BytesIO(b'installer'))
            self.assertEqual(target.read_bytes(), b'installer')
            target.unlink()
            with self.assertRaises(ValueError): download(update, folder, opener=lambda _: io.BytesIO(b'corrupted'))
            self.assertEqual(list(Path(folder).iterdir()), [])
            with self.assertRaises(ValueError): download(update, folder, opener=lambda _: io.BytesIO(b'installer-extra'))
            self.assertEqual(list(Path(folder).iterdir()), [])
    def test_offline_is_reported_without_install(self):
        def offline(url):
            self.assertEqual(url, API)
            raise OSError('offline')
        with self.assertRaises(OSError): check('0.2.0-powerfulmoon.2', offline)
    def test_rejects_untrusted_redirects(self):
        for url in ('http://github.com/test', 'https://evil.invalid/file', 'https://github.com.evil.invalid/file', 'https://user@github.com/file'):
            with self.assertRaises(ValueError): SafeRedirect().redirect_request(None, None, 302, '', {}, url)

if __name__ == '__main__': unittest.main()
