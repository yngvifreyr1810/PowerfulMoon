"""MIT. Download only published PowerfulMoon installers; never upstream binaries."""
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import urllib.parse
import urllib.request
from version_info import version_tuple

REPO = 'yngvifreyr1810/PowerfulMoon'
API = 'https://api.github.com/repos/' + REPO + '/releases/latest'
MAX_SETUP = 800 * 1024 * 1024
CDN = {'github.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com',
       'github-releases.githubusercontent.com', 'api.github.com'}

class SafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        parsed = urllib.parse.urlsplit(newurl)
        if parsed.scheme != 'https' or parsed.hostname not in CDN or parsed.username or parsed.password or parsed.port not in (None, 443):
            raise ValueError('Untrusted download redirect')
        return super().redirect_request(req, fp, code, msg, headers, newurl)

def open_url(url):
    return urllib.request.build_opener(SafeRedirect()).open(
        urllib.request.Request(url, headers={'User-Agent': 'PowerfulMoon-Updater', 'Accept': 'application/vnd.github+json'}), timeout=30)

@dataclass(frozen=True)
class Update:
    version: str
    url: str
    sha256: str
    size: int
    name: str

def parse_release(data, current):
    version_tuple(current)
    if data.get('draft') or data.get('prerelease') or not data.get('published_at'):
        return None
    version = data.get('tag_name', '').removeprefix('v')
    if version_tuple(version) <= version_tuple(current):
        return None
    # File names include the fork revision, so same-upstream-version fixes can update too.
    name = 'PowerfulMoon-Setup-' + version + '.exe'
    assets = [a for a in data.get('assets', []) if a.get('name') == name]
    if len(assets) != 1:
        raise ValueError('The published release has no complete installer for this updater.')
    asset = assets[0]
    expected = 'https://github.com/' + REPO + '/releases/download/v' + version + '/' + name
    digest = asset.get('digest', '')
    size = asset.get('size')
    if asset.get('browser_download_url') != expected or not re.fullmatch(r'sha256:[0-9a-f]{64}', digest):
        raise ValueError('Installer URL or SHA-256 metadata is invalid.')
    if not isinstance(size, int) or isinstance(size, bool) or not 0 < size <= MAX_SETUP:
        raise ValueError('Installer size is invalid.')
    return Update(version, expected, digest[7:], size, name)

def check(current, opener=open_url):
    with opener(API) as response:
        raw = response.read(2 * 1024 * 1024 + 1)
    if len(raw) > 2 * 1024 * 1024:
        raise ValueError('Release metadata is too large.')
    return parse_release(json.loads(raw), current)

def download(update, directory, progress=lambda received, total: None, opener=open_url):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    target = directory / update.name
    part = target.with_suffix('.exe.partial')
    digest = hashlib.sha256()
    received = 0
    try:
        with opener(update.url) as response, part.open('wb') as out:
            while chunk := response.read(1024 * 1024):
                received += len(chunk)
                if received > update.size:
                    raise ValueError('Downloaded installer exceeds its published size.')
                digest.update(chunk)
                out.write(chunk)
                progress(received, update.size)
        if received != update.size or digest.hexdigest() != update.sha256:
            raise ValueError('Installer verification failed. Nothing was installed.')
        os.replace(part, target)
        return target
    finally:
        part.unlink(missing_ok=True)
