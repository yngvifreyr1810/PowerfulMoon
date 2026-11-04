"""The sync bot must retain fork edits and stop on genuine conflicts."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/automation'))
from upstream_sync import allowed, apply_delta, git, latest_stable

class UpstreamBot(unittest.TestCase):
    def test_selects_highest_stable_not_response_order(self):
        releases = [{'tag_name': t, 'commit': {'id': 'a' * 40}} for t in ('v0.1.6', 'v0.2.1', 'v0.2.0', 'v0.3.0-beta')]
        self.assertEqual(latest_stable(releases)['tag_name'], 'v0.2.1')
        self.assertFalse(allowed('.github/workflows/publish-reviewed.yml'))
        self.assertFalse(allowed('tools/windows/installer_core.py'))
        self.assertFalse(allowed('tools/automation/upstream_sync.py'))
    def test_clean_three_way_merge_keeps_fork_and_excludes_workflows(self):
        self.merge(False)
    def test_conflict_stops_without_commit_or_publication(self):
        self.merge(True)
    def merge(self, conflict):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            git(root, 'init')
            git(root, 'config', 'user.name', 'test'); git(root, 'config', 'user.email', 'test@example.invalid')
            (root / 'runtime').mkdir()
            path = root / 'runtime/example.c'
            path.write_text('first\n' + 'middle\n' * 15 + 'last\n')
            git(root, 'add', '.'); git(root, 'commit', '-m', 'base')
            base = git(root, 'rev-parse', 'HEAD').stdout.strip()
            path.write_text(path.read_text().replace('first', 'upstream'))
            workflow = root / '.github/workflows/bad.yml'; workflow.parent.mkdir(parents=True); workflow.write_text('ignored')
            git(root, 'add', '.'); git(root, 'commit', '-m', 'upstream')
            new = git(root, 'rev-parse', 'HEAD').stdout.strip()
            git(root, 'checkout', '-b', 'fork', base)
            path.write_text(path.read_text().replace('first' if conflict else 'last', 'custom-fork'))
            git(root, 'add', '.'); git(root, 'commit', '-m', 'fork')
            fork = git(root, 'rev-parse', 'HEAD').stdout.strip()
            if conflict:
                with self.assertRaisesRegex(RuntimeError, 'Automatic merge stopped'): apply_delta(root, base, new)
                self.assertEqual(git(root, 'rev-parse', 'HEAD').stdout.strip(), fork)
            else:
                selected, ignored = apply_delta(root, base, new)
                self.assertIn('upstream', path.read_text())
                self.assertIn('custom-fork', path.read_text())
                self.assertFalse(workflow.exists())
                self.assertIn('.github/workflows/bad.yml', ignored)

if __name__ == '__main__': unittest.main()
