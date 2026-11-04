"""MIT. Reproduce build inputs on a fresh runner; never read a player's game folder."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import sys
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
BASE = 'https://github.com/yngvifreyr1810/PowerfulMoon/releases/download/v0.2.0-powerfulmoon.2/PowerfulMoon-0.2.0-windows-x64.zip'
BASE_SHA = 'df4fe8f67786906ad84b99f3de5c788655e98df76a573998b24ccfd4628bbe3c'
BRIDGE = 'https://github.com/yngvifreyr1810/ProCon2Bridge/releases/download/v0.4.0/ProCon2Bridge-0.4.0-windows-x64.zip'
BRIDGE_SHA = '6cb9e17d45e2338689009a8fe57344c0160df124f0474bce91c7c1c487a145e6'

def fetch(url, target, digest):
    if target.is_file() and hashlib.file_digest(target.open('rb'), 'sha256').hexdigest() == digest:
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'PowerfulMoon-Builder'}), timeout=120) as response, target.open('wb') as output:
        shutil.copyfileobj(response, output)
    with target.open('rb') as data:
        if hashlib.file_digest(data, 'sha256').hexdigest() != digest:
            target.unlink()
            raise ValueError('Pinned dependency checksum mismatch: ' + target.name)

def main():
    dest = ROOT / 'build/ci-inputs'
    dest.mkdir(parents=True, exist_ok=True)
    base = dest / 'baseline.zip'
    fetch(BASE, base, BASE_SHA)
    runtime = dest / 'runtime'
    runtime.mkdir(exist_ok=True)
    entries = json.loads((ROOT / 'tools/windows/redistributables-0.2.0.json').read_text())['files']
    with zipfile.ZipFile(base) as archive:
        names = [i['path'] for i in entries] + ['libc++.dll']
        names += ['licenses/source/' + i['name'] for i in json.loads((ROOT / 'tools/windows/dependency-sources.json').read_text())]
        for name in names:
            path = PurePosixPath(name)
            if path.is_absolute() or '..' in path.parts or ':' in name or '\\' in name:
                raise ValueError('Unsafe redistributable path')
            target = runtime / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read('PowerfulMoon/' + name))
    fetch(BRIDGE, dest / 'bridge.zip', BRIDGE_SHA)
    # Only SDK headers/import/static libraries. DLLs shipped to players come from
    # the audited baseline above, never from pacman's GPL-enabled FFmpeg build.
    for pkg in json.loads((ROOT / 'tools/windows/build-sdk.json').read_text()):
        name = pkg['FILENAME']
        target = dest / 'packages' / name
        fetch('https://repo.msys2.org/mingw/clang64/' + name, target, pkg['SHA256SUM'])
        with tarfile.open(target, 'r:*') as archive:
            for member in archive:
                parts = PurePosixPath(member.name).parts
                if not member.isfile() or len(parts) < 3 or parts[:2] not in [('clang64', 'include'), ('clang64', 'lib')]:
                    continue
                if '..' in parts or ':' in member.name or '\\' in member.name:
                    raise ValueError('Unsafe SDK member')
                out = dest / 'sdk' / member.name
                out.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, out.open('wb') as output:
                    shutil.copyfileobj(source, output)
    print('Pinned build inputs ready:', dest)

if __name__ == '__main__':
    if sys.version_info < (3, 14):
        raise SystemExit('Python 3.14 is required for pinned .tar.zst SDK packages.')
    main()
