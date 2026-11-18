"""Watch public upstream releases and open tracking issues for review."""

import argparse
import base64
import json
import os
import re
import sys
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


FORK = "yngvifreyr1810/PowerfulMoon"
UPSTREAM = "Simon3Sarkis/super-odyssey-pc-recomp"
TAG_PATTERN = re.compile(r"v[0-9]+\.[0-9]+\.[0-9]+")
SHA_PATTERN = re.compile(r"[0-9a-f]{40}")
INITIAL_FORK_TAG = "v0.4.2.1"
INITIAL_FORK_SHA = "812c625c9a6dd4276226351b78b5610a90462fea"
INITIAL_UPSTREAM_TAG = "v0.4.2"
INITIAL_UPSTREAM_SHA = "a153e72ececf02d95caa691885035cfec2b7990a"


class GitHub:
    def __init__(self, token):
        self.token = token

    def request(self, repo, path, data=None):
        headers = {
            "Accept": "application/vnd.github+json",
            "User-Agent": "PowerfulMoon-Upstream-Watch",
            "X-GitHub-Api-Version": "2022-11-28",
        }
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        if data is not None:
            headers["Content-Type"] = "application/json"
        request = Request(
            f"https://api.github.com/repos/{repo}/{path}",
            headers=headers,
            data=None if data is None else json.dumps(data).encode("utf-8"),
        )
        with urlopen(request, timeout=30) as response:
            return json.load(response)

    def list_all(self, repo, path):
        separator = "&" if "?" in path else "?"
        page = 1
        while True:
            batch = self.request(repo, f"{path}{separator}per_page=100&page={page}")
            yield from batch
            if len(batch) < 100:
                return
            page += 1


def release_version(tag):
    return tuple(map(int, tag[1:].split(".")))


def public_release(release):
    return release.get("draft") is False and release.get("prerelease") is False


def tagged_commit(api, repo, tag):
    obj = api.request(repo, f"git/ref/tags/{tag}")["object"]
    visited = set()
    while obj["type"] == "tag":
        sha = obj["sha"]
        if not SHA_PATTERN.fullmatch(sha) or sha in visited:
            raise ValueError(f"Could not resolve annotated tag {repo}@{tag}")
        visited.add(sha)
        obj = api.request(repo, f"git/tags/{sha}")["object"]
    if obj["type"] != "commit" or not SHA_PATTERN.fullmatch(obj["sha"]):
        raise ValueError(f"{repo}@{tag} does not point to a commit")
    return obj["sha"]


def published_baseline(api):
    release = api.request(FORK, "releases/latest")
    fork_tag = release["tag_name"]
    if not public_release(release) or not re.fullmatch(r"v[0-9]+(?:\.[0-9]+){3}", fork_tag):
        raise ValueError(f"Unexpected latest public PowerfulMoon release: {fork_tag}")
    fork_sha = tagged_commit(api, FORK, fork_tag)

    try:
        file = api.request(FORK, f"contents/release.json?ref={fork_sha}")
    except HTTPError as error:
        error.close()
        if error.code == 404 and (fork_tag, fork_sha) == (INITIAL_FORK_TAG, INITIAL_FORK_SHA):
            return fork_tag, INITIAL_UPSTREAM_TAG, INITIAL_UPSTREAM_SHA
        raise

    if file["encoding"] != "base64":
        raise ValueError(f"Unexpected encoding for release.json at {fork_tag}")
    manifest = json.loads(base64.b64decode(file["content"]))
    upstream_tag = manifest["upstream_tag"]
    upstream_sha = manifest["upstream_commit"]
    if (
        "v" + manifest["version"] != fork_tag
        or upstream_tag != fork_tag.rsplit(".", 1)[0]
        or not TAG_PATTERN.fullmatch(upstream_tag)
        or not SHA_PATTERN.fullmatch(upstream_sha)
    ):
        raise ValueError(f"release.json does not match published tag {fork_tag}")
    return fork_tag, upstream_tag, upstream_sha


def marker(tag):
    return f"<!-- powerfulmoon-upstream-watch:{tag} -->"


def issue_title(tag):
    return f"Upstream {tag}: PowerfulMoon review"


def existing_issue(issues, tag):
    return next(
        (
            issue
            for issue in issues
            if "pull_request" not in issue
            and (
                marker(tag) in (issue.get("body") or "").splitlines()
                or issue["title"] == issue_title(tag)
            )
        ),
        None,
    )


def watch(api, notify=False):
    fork_tag, base_tag, base_sha = published_baseline(api)
    print(f"Published baseline: PowerfulMoon {fork_tag}, upstream {base_tag} ({base_sha})")
    newer = set()
    for release in api.list_all(UPSTREAM, "releases"):
        tag = release["tag_name"]
        if public_release(release) and TAG_PATTERN.fullmatch(tag):
            if release_version(tag) > release_version(base_tag):
                newer.add(tag)

    if not newer:
        print("No newer public upstream releases.")
        return

    issues = list(api.list_all(FORK, "issues?state=all"))
    page = f"https://github.com/{UPSTREAM}"
    for tag in sorted(newer, key=release_version):
        issue = existing_issue(issues, tag)
        if issue is not None:
            print(f"Already tracked: {tag} ({issue['html_url']})")
            continue
        sha = tagged_commit(api, UPSTREAM, tag)
        body = (
            f"{marker(tag)}\n"
            f"Upstream release: [{tag}]({page}/releases/tag/{tag})\n\n"
            f"Upstream commit: [`{sha}`]({page}/commit/{sha})\n\n"
            f"Published PowerfulMoon baseline: [`{fork_tag}`]"
            f"(https://github.com/{FORK}/releases/tag/{fork_tag}), "
            f"based on upstream `{base_tag}` (`{base_sha}`).\n\n"
            f"[Compare the pinned upstream commits]({page}/compare/{base_sha}...{sha}).\n\n"
            "Review the changelog, commit diff, and affected fork patches before choosing an update path."
        )
        if notify:
            created = api.request(FORK, "issues", {"title": issue_title(tag), "body": body})
            print(f"Created: {created['html_url']}")
        else:
            print(f"Would create: {issue_title(tag)}\n{body}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true", help="report only (default)")
    mode.add_argument("--notify", action="store_true", help="create missing issue notices")
    args = parser.parse_args()
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if args.notify and not token:
        parser.error("--notify requires GITHUB_TOKEN or GH_TOKEN")
    if args.notify and os.environ.get("GITHUB_REPOSITORY", FORK).casefold() != FORK.casefold():
        parser.error(f"--notify is only supported in {FORK}")
    try:
        watch(GitHub(token), notify=args.notify)
    except (KeyError, TypeError, ValueError, URLError) as error:
        if isinstance(error, HTTPError):
            error.close()
        print(f"Upstream watch failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
