import base64
import contextlib
import io
import json
import unittest
from urllib.error import HTTPError

import upstream_watch as watch


def release(tag, *, draft=False, prerelease=False):
    return {"tag_name": tag, "draft": draft, "prerelease": prerelease}


class FakeGitHub(watch.GitHub):
    def __init__(self):
        self.posts = []
        self.calls = []
        self.pages = {}
        self.responses = {
            (watch.FORK, "releases/latest"): release(watch.INITIAL_FORK_TAG),
            (watch.FORK, f"git/ref/tags/{watch.INITIAL_FORK_TAG}"): {
                "object": {"type": "commit", "sha": watch.INITIAL_FORK_SHA}
            },
            (watch.FORK, f"contents/release.json?ref={watch.INITIAL_FORK_SHA}"): None,
        }

    def request(self, repo, path, data=None):
        self.calls.append((repo, path))
        if data is not None:
            self.posts.append((repo, path, data))
            return {"html_url": f"https://github.com/{repo}/issues/1"}
        response = self.responses.get((repo, path), self.pages.get((repo, path), []))
        if response is None:
            raise HTTPError("", 404, "Not Found", None, io.BytesIO())
        return response

    def tags(self, *tags):
        self.pages[(watch.UPSTREAM, "releases?per_page=100&page=1")] = list(tags)
        for item in tags:
            self.responses[(watch.UPSTREAM, f"git/ref/tags/{item['tag_name']}")] = {
                "object": {"type": "commit", "sha": "4" * 40}
            }


class UpstreamWatchTest(unittest.TestCase):
    def setUp(self):
        self.api = FakeGitHub()

    def run_watch(self, notify=False):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            watch.watch(self.api, notify=notify)
        return out.getvalue()

    def test_dry_run_reports_numeric_newer_public_tags(self):
        self.api.tags(
            release("v0.4.10"),
            release("v0.4.3"),
            release("v0.4.2"),
            release("v0.4.1"),
            release("v0.5.0", prerelease=True),
            release("v0.6.0", draft=True),
            release("v1.0.0-alpha"),
        )
        out = self.run_watch()
        self.assertEqual(self.api.posts, [])
        self.assertIn("PowerfulMoon v0.4.2.1, upstream v0.4.2", out)
        self.assertLess(out.index("Would create: Upstream v0.4.3"), out.index("Would create: Upstream v0.4.10"))
        self.assertNotIn("v0.5.0", out)
        self.assertNotIn("v0.6.0", out)
        self.assertNotIn("v1.0.0-alpha", out)
        self.assertIn(f"/compare/{watch.INITIAL_UPSTREAM_SHA}...{'4' * 40}", out)

    def test_manifest_loaded_at_latest_release_commit(self):
        latest = "v0.4.3.2"
        fork_sha = "1" * 40
        upstream_sha = "2" * 40
        self.api.responses[(watch.FORK, "releases/latest")] = release(latest)
        self.api.responses[(watch.FORK, f"git/ref/tags/{latest}")] = {
            "object": {"type": "commit", "sha": fork_sha}
        }
        manifest = {"version": "0.4.3.2", "upstream_tag": "v0.4.3", "upstream_commit": upstream_sha}
        self.api.responses[(watch.FORK, f"contents/release.json?ref={fork_sha}")] = {
            "content": base64.b64encode(json.dumps(manifest).encode()).decode(),
            "encoding": "base64",
        }
        self.api.tags(release("v0.4.3"), release("v0.4.4"))
        out = self.run_watch()
        self.assertNotIn("Would create: Upstream v0.4.3", out)
        self.assertIn(f"/compare/{upstream_sha}...{'4' * 40}", out)

    def test_bootstrap_requires_the_original_fork_tag_commit(self):
        replaced = "1" * 40
        self.api.responses[(watch.FORK, f"git/ref/tags/{watch.INITIAL_FORK_TAG}")] = {
            "object": {"type": "commit", "sha": replaced}
        }
        self.api.responses[(watch.FORK, f"contents/release.json?ref={replaced}")] = None
        with self.assertRaises(HTTPError):
            self.run_watch()
        self.assertEqual(self.api.posts, [])

    def test_notify_peels_annotated_upstream_tag(self):
        tag = "v0.4.3"
        tag_object = "7" * 40
        self.api.tags(release(tag))
        self.api.responses[(watch.UPSTREAM, f"git/ref/tags/{tag}")] = {
            "object": {"type": "tag", "sha": tag_object}
        }
        self.api.responses[(watch.UPSTREAM, f"git/tags/{tag_object}")] = {
            "object": {"type": "commit", "sha": "5" * 40}
        }
        self.run_watch(notify=True)
        [(repo, endpoint, data)] = self.api.posts
        self.assertEqual((watch.FORK, "issues"), (repo, endpoint))
        self.assertEqual(watch.issue_title(tag), data["title"])
        self.assertIn(watch.marker(tag), data["body"])
        self.assertIn(f"/compare/{watch.INITIAL_UPSTREAM_SHA}...{'5' * 40}", data["body"])
        self.assertIn(f"/releases/tag/{tag}", data["body"])

    def test_deduplicates_closed_renamed_and_legacy_issues_across_pages(self):
        self.api.tags(release("v0.4.3"), release("v0.4.4"), release("v0.4.5"))
        prefix = "issues?state=all&per_page=100&page="
        self.api.pages[(watch.FORK, prefix + "1")] = [{"title": "Unrelated", "body": None}] * 100
        self.api.pages[(watch.FORK, prefix + "2")] = [
            {"title": "Renamed", "body": watch.marker("v0.4.3"), "state": "closed", "html_url": "issue-1"},
            {"title": watch.issue_title("v0.4.4"), "body": None, "html_url": "issue-2"},
            {"title": watch.issue_title("v0.4.5"), "body": None, "pull_request": {}},
        ]
        out = self.run_watch(notify=True)
        self.assertIn("Already tracked: v0.4.3", out)
        self.assertIn("Already tracked: v0.4.4", out)
        [(_, _, data)] = self.api.posts
        self.assertEqual(data["title"], watch.issue_title("v0.4.5"))


if __name__ == "__main__":
    unittest.main()
