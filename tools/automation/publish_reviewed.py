"""MIT. Manually approved promotion of immutable build artifacts, without rebuilding."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import urllib.parse
import urllib.request
import zipfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'windows'))
from version_info import version_tuple
from upstream_sync import GitHub

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None

def artifact_download(api, artifact_id, path):
    req = urllib.request.Request('https://api.github.com/repos/' + api.repo + '/actions/artifacts/' + str(artifact_id) + '/zip',
          headers={'Authorization': 'Bearer ' + api.token, 'User-Agent': 'PowerfulMoon-Release'})
    try:
        response = urllib.request.build_opener(NoRedirect()).open(req, timeout=60)
    except urllib.error.HTTPError as error:
        if error.code != 302: raise
        url = error.headers['Location']
        if urllib.parse.urlsplit(url).scheme != 'https': raise ValueError('Invalid artifact redirect')
        # Never forward the repository token to Azure's signed artifact URL.
        response = urllib.request.urlopen(url, timeout=120)
    with response, path.open('wb') as output:
        shutil.copyfileobj(response, output)

def verified_files(folder, sha):
    meta = json.loads((folder / 'release.json').read_text())
    version = meta['version']; version_tuple(version)
    zipname = 'PowerfulMoon-' + version + '-windows-x64.zip'
    exename = 'PowerfulMoon-Setup-' + version + '.exe'
    expected = {zipname, exename, zipname + '.sha256', exename + '.sha256', 'PowerfulMoon-content-audit.json', 'release.json'}
    if {p.name for p in folder.iterdir()} != expected:
        raise ValueError('Unexpected or incomplete candidate artifact')
    audit = json.loads((folder / 'PowerfulMoon-content-audit.json').read_text())
    if audit['source_commit'] != sha or audit['forbidden_paths_found'] or not audit['embedded_setup_payload_matches_zip'] or audit['game_files_keys_saves_generated_game_library_included']:
        raise ValueError('Candidate content audit does not match the reviewed commit')
    for name, field in ((zipname, 'zip_sha256'), (exename, 'setup_sha256')):
        with (folder / name).open('rb') as file:
            digest = hashlib.file_digest(file, 'sha256').hexdigest()
        if digest != audit[field] or (folder / (name + '.sha256')).read_text().split() != [digest, name]:
            raise ValueError('Candidate checksum mismatch: ' + name)
    with zipfile.ZipFile(folder / zipname) as archive:
        manifest = json.loads(archive.read('PowerfulMoon/PACKAGE.json'))
        if manifest['source_commit'] != sha or manifest['version'] != version or manifest['name'] != 'PowerfulMoon':
            raise ValueError('Package identity differs from reviewed source')
    return meta, [folder / name for name in sorted(expected - {'release.json'})]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--run', required=True, type=int)
    parser.add_argument('--sha', required=True)
    parser.add_argument('--confirmation', required=True)
    args = parser.parse_args()
    if args.confirmation != 'PUBLISH' or not re.fullmatch(r'[0-9a-f]{40}', args.sha):
        raise ValueError('Explicit PUBLISH approval and exact commit are required')
    repo = os.environ['GITHUB_REPOSITORY']
    if os.environ['GITHUB_ACTOR'].lower() != repo.split('/')[0].lower():
        raise ValueError('Only the repository owner may promote a candidate')
    api = GitHub(repo, os.environ['GITHUB_TOKEN'])
    run = api.call('actions/runs/' + str(args.run))
    if run['conclusion'] != 'success' or run['event'] not in ('workflow_dispatch', 'push') or run['path'] != '.github/workflows/candidate.yml':
        raise ValueError('A successful candidate workflow run is required')
    artifacts = api.call('actions/runs/' + str(args.run) + '/artifacts')['artifacts']
    matches = [a for a in artifacts if not a['expired'] and a['name'] == 'PowerfulMoon-candidate-' + args.sha]
    if len(matches) != 1: raise ValueError('No unique unexpired artifact for the approved commit')
    with tempfile.TemporaryDirectory() as temp:
        folder = Path(temp); archive = folder / 'artifact.zip'
        artifact_download(api, matches[0]['id'], archive)
        payload = folder / 'payload'; payload.mkdir()
        with zipfile.ZipFile(archive) as source:
            for item in source.infolist():
                if item.filename != Path(item.filename).name or '\\' in item.filename or ':' in item.filename or item.is_dir():
                    raise ValueError('Unexpected artifact path')
                with source.open(item) as file, (payload / item.filename).open('wb') as out:
                    shutil.copyfileobj(file, out)
        meta, files = verified_files(payload, args.sha)
        version = meta['version']; tag = 'v' + version
        body = ('Customized fork of [Power Moon Recomp](https://gitlab.com/zzzarathustra-dev/power-moon-recomp) ' + meta['upstream_tag'] + '.\n\n'
                'Includes the upstream update, Switch 2 Pro Controller Bluetooth support through the bundled ProCon2Bridge, persistent controller settings, and the VRR interpolation/VSync controls. '
                'The Update button offers only approved PowerfulMoon releases.\n\n'
                '**Upgrade:** close the game and select your existing game folder in Setup. Saves, mods and settings are retained; replaced program files are backed up under `powerfulmoon-backups/`.\n\n'
                'Download **PowerfulMoon-Setup-' + version + '.exe** for installation, or the Windows ZIP for portable use. '
                'Game files, keys and Nintendo assets are not included. Supply your own game files.\n\n'
                '[Credits](https://github.com/' + repo + '/blob/' + args.sha + '/CREDITS.md) · '
                '[Security](https://github.com/' + repo + '/blob/' + args.sha + '/SECURITY.md)\n\n'
                'Source: `' + args.sha + '`')
        # A draft remains invisible to the app until every verified asset is uploaded.
        release = api.call('releases', {'tag_name': tag, 'target_commitish': args.sha, 'name': 'PowerfulMoon ' + version,
                                       'body': body, 'draft': True, 'prerelease': False})
        upload = release['upload_url'].split('{')[0]
        for path in files:
            # urllib uses the file object as a streaming body with an explicit length.
            with path.open('rb') as data:
                request = urllib.request.Request(upload + '?name=' + urllib.parse.quote(path.name), data=data,
                    headers={'Authorization': 'Bearer ' + api.token, 'User-Agent': 'PowerfulMoon-Release',
                             'Content-Type': 'application/octet-stream', 'Content-Length': str(path.stat().st_size)}, method='POST')
                with urllib.request.urlopen(request, timeout=300) as response: response.read()
        api.call('releases/' + str(release['id']), {'draft': False}, 'PATCH')
        print('Published reviewed candidate:', release['html_url'])

if __name__ == '__main__':
    main()
