#!/usr/bin/env python3
"""Check release contents without installing the launcher or extracting ZIPs."""

import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import tarfile
import zipfile


DATA_DIRECTORIES = {
    "cache", "save", "saves", "save-cap", "extracted", "exefs", "romfs", "mods", "reports",
}
PRIVATE_FILES = {
    "data-folder.txt", "powermoon.ini", "settings.ini", "superodysseyrecomp.log",
    "version.dll", "dlssg_sm86.ini", "prod.keys", "title.keys", "dev.keys", "romfs.bin", "main.npdm",
}
GENERATED_FILES = {
    "pipeline-cache", "pipeline_cache", "graphics-pipelines", "vk_pipelines",
    "shaders.pack", "shader-cache", "shader_cache", "shadercache",
    "drawn_programs", "first-scene-pipelines", "prepared-compute", "translations.bin",
}
GAME_SUFFIXES = {".nsp", ".nsz", ".xci", ".xcz", ".nca", ".nro", ".nso", ".sav", ".bfsar", ".keys"}
ZIG_CACHE_HEADERS = ("zig", "lib", "std", "build", "cache")


class AuditError(ValueError):
    """The package does not meet the release policy."""


def member_path(name):
    if name.startswith("./"):
        name = name[2:]
    if name.endswith("/"):
        name = name[:-1]
    parts = tuple(name.split("/"))
    if any(not part or part in {".", ".."} or part.endswith((" ", ".")) for part in parts):
        raise AuditError(f"unsafe package path: {name!r}")
    if any(char in name for char in '\\:\0\n\r'):
        raise AuditError(f"unsafe package path: {name!r}")
    return parts


def forbidden(parts):
    lowered = tuple(part.casefold() for part in parts)
    name = lowered[-1]
    if any(part in DATA_DIRECTORIES for part in lowered):
        # Zig ships these compiler sources under a directory named Cache.
        if not (
            lowered[:5] == ZIG_CACHE_HEADERS
            and len(lowered) in {5, 6}
            and (len(lowered) == 5 or name.endswith(".zig"))
        ):
            return True
    return (
        name in PRIVATE_FILES
        or any(name == stem or name.startswith(stem + ".") for stem in GENERATED_FILES)
        or name.startswith("odyssey_game.")
        or PurePosixPath(name).suffix in GAME_SUFFIXES
    )


def digest(stream):
    checksum = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        checksum.update(chunk)
    return checksum.hexdigest()


def audit_members(entries, kind, seed_sha256, open_member):
    """Audit (path, regular file status, handle) entries and the sole seed."""
    entries = list(entries)
    files = {parts for parts, regular, _ in entries if regular}
    if kind == "Windows ZIP":
        launcher = ("SuperOdysseyRecomp.exe",)
        seed = ("pipeline-seed.bin.zst",)
        required = (launcher, ("odysseygen", "python.exe"))
    elif kind == "source archive":
        launcher = ("runtime", "version.h")
        seed = ("data", "pipeline-seed.bin.zst")
        required = (launcher,)
    else:
        launcher = ("usr", "bin", "SuperOdysseyRecomp")
        seed = ("usr", "bin", "pipeline-seed.bin.zst")
        required = (launcher, ("AppRun",), ("usr", "bin", "odysseygen", "odysseygen"))

    roots = {parts[: -len(launcher)] for parts in files if parts[-len(launcher) :] == launcher}
    if len(roots) != 1:
        raise AuditError(f"{kind}: expected exactly one launcher/source root")
    root = roots.pop()
    if (kind == "AppImage" and root) or (kind != "AppImage" and len(root) > 1):
        raise AuditError(f"{kind}: unexpected archive layout")
    missing = ["/".join(relative) for relative in required if root + relative not in files]
    if missing:
        raise AuditError(f"{kind}: missing {', '.join(missing)}")

    seen = set()
    seeds = []
    for parts, regular, handle in entries:
        relative = parts[len(root) :]
        if parts[:len(root)] != root:
            raise AuditError(f"{kind}: content outside the package root: {'/'.join(parts)}")
        if not relative:
            continue
        folded = tuple(part.casefold() for part in relative)
        if folded in seen:
            raise AuditError(f"{kind}: duplicate package entry: {'/'.join(parts)}")
        seen.add(folded)
        runtime_path = relative[2:] if relative[:2] == ("usr", "bin") else relative
        if forbidden(runtime_path or relative):
            raise AuditError(f"{kind}: forbidden package entry: {'/'.join(parts)}")
        if folded[-1] in {"pipeline-seed.bin", "pipeline-seed.bin.zst"}:
            if relative != seed or not regular:
                raise AuditError(f"{kind}: misplaced or non-regular shader seed: {'/'.join(parts)}")
            seeds.append(handle)
    if len(seeds) != 1:
        raise AuditError(f"{kind}: expected one upstream shader seed at {'/'.join(seed)}")
    with open_member(seeds[0]) as stream:
        actual = digest(stream)
    if actual != seed_sha256:
        raise AuditError(f"{kind}: shader seed SHA-256 {actual} does not match {seed_sha256}")
    return len(seen)


def check_zip(path, kind, seed_sha256):
    with zipfile.ZipFile(path) as archive:
        entries = []
        for entry in archive.infolist():
            mode = stat.S_IFMT(entry.external_attr >> 16) if entry.create_system == 3 else 0
            if mode not in (0, stat.S_IFDIR, stat.S_IFREG):
                raise AuditError(f"{kind}: unsupported ZIP entry: {entry.filename!r}")
            entries.append((member_path(entry.orig_filename), not entry.is_dir() and mode != stat.S_IFDIR, entry))
        return audit_members(entries, kind, seed_sha256, archive.open)


def check_source(path, seed_sha256):
    if zipfile.is_zipfile(path):
        return check_zip(path, "source archive", seed_sha256)
    with tarfile.open(path, "r:*") as archive:
        entries = []
        for entry in archive:
            if not entry.isfile() and not entry.isdir():
                raise AuditError(f"source archive: unsupported tar entry: {entry.name!r}")
            if entry.isdir() and entry.name in {".", "./"}:
                continue
            entries.append((member_path(entry.name), entry.isfile(), entry))
        return audit_members(entries, "source archive", seed_sha256, archive.extractfile)


def check_appimage_root(path, seed_sha256):
    if path.is_symlink() or not path.is_dir():
        raise AuditError("AppImage: expected an extracted AppDir directory")
    root = path.resolve()
    entries = []
    def directory_error(error):
        raise error

    for directory, names, filenames in os.walk(root, onerror=directory_error):
        for name in names + filenames:
            entry = Path(directory) / name
            parts = member_path(entry.relative_to(root).as_posix())
            mode = entry.lstat().st_mode
            if stat.S_ISLNK(mode):
                target = os.readlink(entry)
                if Path(target).is_absolute() or not entry.resolve(strict=True).is_relative_to(root):
                    raise AuditError(f"AppImage: symlink escapes the package: {'/'.join(parts)}")
            elif not stat.S_ISDIR(mode) and not stat.S_ISREG(mode):
                raise AuditError(f"AppImage: unsupported file type: {'/'.join(parts)}")
            entries.append((parts, stat.S_ISREG(mode), entry))
    return audit_members(entries, "AppImage", seed_sha256, lambda path: path.open("rb"))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--windows-zip", type=Path)
    parser.add_argument("--appimage-root", type=Path, help="AppDir from the final AppImage")
    parser.add_argument("--source-archive", type=Path, help="source ZIP or tar archive")
    seed = parser.add_mutually_exclusive_group(required=True)
    seed.add_argument("--seed-reference", type=Path, help="pipeline-seed.bin.zst from the pinned upstream commit")
    seed.add_argument("--seed-sha256", help="SHA-256 of the pinned upstream pipeline-seed.bin.zst")
    args = parser.parse_args(argv)
    if not any((args.windows_zip, args.appimage_root, args.source_archive)):
        parser.error("specify at least one package to audit")
    if args.seed_sha256 and not re.fullmatch(r"[0-9a-fA-F]{64}", args.seed_sha256):
        parser.error("--seed-sha256 must be a 64-digit hexadecimal hash")

    try:
        if args.seed_reference:
            with args.seed_reference.open("rb") as stream:
                expected = digest(stream)
        else:
            expected = args.seed_sha256.lower()
        checks = (
            (args.windows_zip, "Windows ZIP", lambda path, sha: check_zip(path, "Windows ZIP", sha)),
            (args.appimage_root, "AppImage", check_appimage_root),
            (args.source_archive, "source archive", check_source),
        )
        for path, label, check in checks:
            if path is not None:
                count = check(path, expected)
                print(f"{label}: checked {count} entries, upstream shader seed SHA-256 {expected}")
    except (AuditError, OSError, RuntimeError, zipfile.BadZipFile, tarfile.TarError) as error:
        print(f"Release audit failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
