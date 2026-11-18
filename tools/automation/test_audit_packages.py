import hashlib
import io
from pathlib import Path
import stat
import tarfile
import tempfile
import unittest
import zipfile

from audit_packages import AuditError, check_appimage_root, check_source, check_zip


SEED = b"test pipeline seed"
SHA = hashlib.sha256(SEED).hexdigest()
WINDOWS_FILES = {
    "Super Odyssey PC Recomp/SuperOdysseyRecomp.exe": b"launcher",
    "Super Odyssey PC Recomp/odysseygen/python.exe": b"runtime",
    "Super Odyssey PC Recomp/pipeline-seed.bin": SEED,
}
SOURCE_FILES = {"runtime/version.h": b"header", "data/pipeline-seed.bin": SEED}
LINUX_FILES = {
    "AppRun": b"bootstrap",
    "usr/bin/SuperOdysseyRecomp": b"launcher",
    "usr/bin/odysseygen/odysseygen": b"generator",
    "usr/bin/pipeline-seed.bin": SEED,
    "usr/bin/zig/lib/std/Build/Cache/Path.zig": b"compiler source",
}


class AuditPackagesTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def write_zip(self, files):
        path = self.root / "package.zip"
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in files.items():
                archive.writestr(name, data)
        return path

    def write_appdir(self, files):
        appdir = self.root / "squashfs-root"
        for name, data in files.items():
            target = appdir / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        return appdir

    def test_accepts_launcher_and_matching_seed(self):
        win = self.write_zip(WINDOWS_FILES)
        self.assertEqual(check_zip(win, "Windows ZIP", SHA), len(WINDOWS_FILES))
        self.assertEqual(check_source(self.write_zip(SOURCE_FILES), SHA), len(SOURCE_FILES))
        self.assertGreaterEqual(check_appimage_root(self.write_appdir(LINUX_FILES), SHA), len(LINUX_FILES))

    def test_accepts_github_style_source_tar(self):
        tar = self.root / "source.tar.gz"
        with tarfile.open(tar, "w:gz") as archive:
            for name, data in SOURCE_FILES.items():
                entry = tarfile.TarInfo("PowerfulMoon-1234567/" + name)
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
        self.assertEqual(check_source(tar, SHA), len(SOURCE_FILES))

    def test_rejects_nested_player_data_and_cache(self):
        for member in (
            "data/shaders.pack", "data/vk_pipelines.bin", "data/graphics-pipelines.bin",
            "data/pipeline-cache.bin", "data/odyssey_game.dll.id", "data/save/slot0",
            "data/keys/prod.keys", "extra/dump.XCI", "extra/version.dll", "settings.ini",
        ):
            with self.subTest(member=member):
                win = self.write_zip({**WINDOWS_FILES, f"Super Odyssey PC Recomp/{member}": b"x"})
                with self.assertRaisesRegex(AuditError, "forbidden package entry"):
                    check_zip(win, "Windows ZIP", SHA)

    def test_rejects_bad_and_duplicate_seed(self):
        seed = "Super Odyssey PC Recomp/pipeline-seed.bin"
        for files, message in (
            ({name: body for name, body in WINDOWS_FILES.items() if name != seed}, "expected one upstream shader seed"),
            ({**WINDOWS_FILES, seed: b"wrong"}, "does not match"),
            ({**WINDOWS_FILES, "Super Odyssey PC Recomp/data/pipeline-seed.bin": SEED}, "misplaced"),
        ):
            with self.subTest(message=message):
                with self.assertRaisesRegex(AuditError, message):
                    check_zip(self.write_zip(files), "Windows ZIP", SHA)

    def test_rejects_unexpected_source_or_linux_payloads(self):
        with self.assertRaisesRegex(AuditError, "forbidden package entry"):
            check_source(self.write_zip({**SOURCE_FILES, "bin/odyssey_game.so": b"x"}), SHA)
        appdir = self.write_appdir({**LINUX_FILES, "usr/bin/shaders.pack": b"cache"})
        with self.assertRaisesRegex(AuditError, "forbidden package entry"):
            check_appimage_root(appdir, SHA)

    def test_rejects_traversal_ambiguous_paths_and_links_in_zip(self):
        traversal = {**WINDOWS_FILES, "Super Odyssey PC Recomp/../hidden.bin": b"x"}
        with self.assertRaisesRegex(AuditError, "unsafe package path"):
            check_zip(self.write_zip(traversal), "Windows ZIP", SHA)
        duplicate = {**WINDOWS_FILES, "Super Odyssey PC Recomp/ODYSSEYGEN/PYTHON.EXE": b"x"}
        with self.assertRaisesRegex(AuditError, "duplicate package entry"):
            check_zip(self.write_zip(duplicate), "Windows ZIP", SHA)
        zx = self.write_zip(WINDOWS_FILES)
        with zipfile.ZipFile(zx, "a") as archive:
            link = zipfile.ZipInfo("Super Odyssey PC Recomp/link")
            link.create_system = 3
            link.external_attr = stat.S_IFLNK << 16
            archive.writestr(link, "../outside")
        with self.assertRaisesRegex(AuditError, "unsupported ZIP entry"):
            check_zip(zx, "Windows ZIP", SHA)


if __name__ == "__main__":
    unittest.main()
