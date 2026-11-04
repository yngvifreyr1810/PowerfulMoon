"""MIT. Deterministic upstream candidate bot. No AI, no releases, no game files."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import urllib.error
import urllib.request

UPSTREAM = 'https://gitlab.com/zzzarathustra-dev/power-moon-recomp.git'
RELEASES = 'https://gitlab.com/api/v4/projects/zzzarathustra-dev%2Fpower-moon-recomp/releases?per_page=100'
ROOT = Path(__file__).resolve().parents[2]

def git(root, *args, binary=False, check=True):
    return subprocess.run(['git', '-C', str(root), *args], check=check, capture_output=True, text=not binary)

def latest_stable(releases):
    releases = [r for r in releases if re.fullmatch(r'v\d+\.\d+\.\d+', r.get('tag_name', ''))
                and not r.get('upcoming_release') and re.fullmatch(r'[0-9a-f]{40}', r.get('commit', {}).get('id', ''))]
    if not releases:
        raise ValueError('No stable upstream release with a pinned source commit')
    return max(releases, key=lambda r: tuple(map(int, r['tag_name'][1:].split('.'))))

def allowed(path):
    # Upstream never gets to replace bot, installer, updater or workflow code.
    return (path.startswith(('runtime/', 'third_party/')) or path == 'Makefile'
            or (path.startswith('tools/') and path.count('/') == 1))

def apply_delta(root, base, new):
    names = git(root, 'diff', '--name-only', '--no-renames', base, new).stdout.splitlines()
    selected = [p for p in names if allowed(p)]
    ignored = [p for p in names if p not in selected]
    # Licensing changes and build/dependency declarations need human review.
    blocked = [p for p in ignored if re.search(r'license|copying|\.gitmodules|cmakelists|requirements|\.lock$', p, re.I)]
    if blocked:
        raise RuntimeError('Upstream legal/dependency changes require review: ' + ', '.join(blocked))
    resource = 'runtime/platform/windows.rc'
    if resource in selected:
        def without_version(text):
            return '\n'.join(line for line in text.splitlines() if not re.search(
                r'^\s*(?:FILEVERSION|PRODUCTVERSION)\b|VALUE "(?:FileVersion|ProductVersion)"', line))
        before = git(root, 'show', base + ':' + resource).stdout
        after = git(root, 'show', new + ':' + resource).stdout
        if without_version(before) != without_version(after):
            raise RuntimeError('Upstream Windows branding/resource changes require review: ' + resource)
        # Fork-owned identity is generated from release.json. Reapplying upstream's
        # numeric/string version bump would create an avoidable conflict every time.
        selected.remove(resource)
        ignored.append(resource + ' (version-only; regenerated for PowerfulMoon)')
    if selected:
        patch = git(root, 'diff', '--binary', '--no-renames', base, new, '--', *selected, binary=True).stdout
        result = subprocess.run(['git', '-C', str(root), 'apply', '--3way', '-'], input=patch, capture_output=True)
        if result.returncode:
            conflicts = git(root, 'diff', '--name-only', '--diff-filter=U').stdout.strip()
            raise RuntimeError('Automatic merge stopped. Review conflicts/apply errors:\n' + conflicts + '\n' + result.stderr.decode(errors='replace')[-6000:])
    return selected, ignored

class GitHub:
    def __init__(self, repo, token):
        self.repo, self.token = repo, token
    def call(self, path, data=None, method=None):
        req = urllib.request.Request('https://api.github.com/repos/' + self.repo + '/' + path,
              data=None if data is None else json.dumps(data).encode(), method=method,
              headers={'Authorization': 'Bearer ' + self.token, 'User-Agent': 'PowerfulMoon-Upstream-Bot',
                       'Accept': 'application/vnd.github+json', 'Content-Type': 'application/json'})
        with urllib.request.urlopen(req, timeout=60) as response:
            raw = response.read()
        return json.loads(raw) if raw else None
    def issue(self, tag, body):
        title = 'Upstream ' + tag + ': PowerfulMoon review'
        issues = self.call('issues?state=all&per_page=100')
        existing = next((i for i in issues if i['title'] == title and 'pull_request' not in i), None)
        if existing:
            if existing['body'] != body:
                self.call('issues/' + str(existing['number']), {'body': body}, 'PATCH')
        else:
            self.call('issues', {'title': title, 'body': body})

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    with urllib.request.urlopen(urllib.request.Request(RELEASES, headers={'User-Agent': 'PowerfulMoon-Upstream-Bot'}), timeout=60) as response:
        release = latest_stable(json.load(response))
    current = json.loads((ROOT / 'release.json').read_text())
    tag, commit = release['tag_name'], release['commit']['id']
    if tuple(map(int, tag[1:].split('.'))) <= tuple(map(int, current['upstream_tag'][1:].split('.'))):
        print('Already integrated:', current['upstream_tag']); return
    repo = os.environ.get('GITHUB_REPOSITORY', 'yngvifreyr1810/PowerfulMoon')
    api = None if args.dry_run else GitHub(repo, os.environ['GITHUB_TOKEN'])
    branch = 'automation/upstream-' + tag
    if api:
        try:
            existing = api.call('git/ref/heads/' + branch)
        except urllib.error.HTTPError as error:
            if error.code != 404: raise
        else:
            sha = existing['object']['sha']
            issues = api.call('issues?state=all&per_page=100')
            recorded = any(i['title'] == 'Upstream ' + tag + ': PowerfulMoon review'
                           and sha in (i.get('body') or '') and 'Build/test requested' in (i.get('body') or '') for i in issues)
            if not recorded:
                # Recover if the previous run pushed the branch but dispatch/notification failed.
                api.call('actions/workflows/candidate.yml/dispatches', {'ref': 'main', 'inputs': {'candidate_sha': sha}})
                api.issue(tag, 'Candidate commit: `' + sha + '`\n\nBuild/test requested: https://github.com/' + repo + '/actions/workflows/candidate.yml\n\nNot published. Owner review is required.')
            print('Candidate already exists:', branch); return
    report = 'Upstream: https://gitlab.com/zzzarathustra-dev/power-moon-recomp/-/releases/' + tag + '\n\nSource: `' + commit + '`\n\n'
    try:
        with tempfile.TemporaryDirectory(prefix='powerfulmoon-sync-') as folder:
            work = Path(folder) / 'repo'
            subprocess.run(['git', 'clone', '--no-hardlinks', '--quiet', str(ROOT), str(work)], check=True)
            git(work, 'config', 'user.name', 'PowerfulMoon upstream bot')
            git(work, 'config', 'user.email', '41898282+github-actions[bot]@users.noreply.github.com')
            for source in (current['upstream_commit'], commit):
                git(work, 'fetch', '--depth=1', UPSTREAM, source)
            selected, ignored = apply_delta(work, current['upstream_commit'], commit)
            meta = {'version': tag[1:] + '-powerfulmoon.1', 'upstream_tag': tag, 'upstream_commit': commit}
            (work / 'release.json').write_text(json.dumps(meta, indent=2) + '\n')
            subprocess.run([os.sys.executable, str(work / 'tools/windows/version_info.py')], check=True, cwd=work)
            git(work, 'add', '--all')
            git(work, 'commit', '-m', 'Prepare upstream ' + tag + ' candidate (requires review)')
            sha = git(work, 'rev-parse', 'HEAD').stdout.strip()
            report += 'Candidate commit: `' + sha + '`\n\nChanged code: ' + ', '.join(selected) + '\n\nUpstream documentation outside merge scope: ' + ', '.join(ignored) + '\n\n'
            if api:
                # A per-process header; credentials are never stored in the candidate checkout.
                import base64
                env = os.environ.copy()
                env.update(GIT_CONFIG_COUNT='1', GIT_CONFIG_KEY_0='http.https://github.com/.extraheader',
                           GIT_CONFIG_VALUE_0='Authorization: Basic ' + base64.b64encode(('x-access-token:' + api.token).encode()).decode(), GIT_TERMINAL_PROMPT='0')
                subprocess.run(['git', '-C', str(work), 'push', 'https://github.com/' + repo + '.git', 'HEAD:refs/heads/' + branch], check=True, env=env)
                api.call('actions/workflows/candidate.yml/dispatches', {'ref': 'main', 'inputs': {'candidate_sha': sha}})
                report += 'Build/test requested: https://github.com/' + repo + '/actions/workflows/candidate.yml\n\n'
            report += '**Not published.** Review the exact commit and build artifact. A separate owner-approved release is required before the app can offer this update.'
    except Exception as error:
        report += '**Needs manual repair. No candidate was released.**\n\n```text\n' + str(error)[:6500].replace('```', "'''") + '\n```'
        if api: api.issue(tag, report)
        raise
    if api: api.issue(tag, report)
    print(report)

if __name__ == '__main__':
    main()
