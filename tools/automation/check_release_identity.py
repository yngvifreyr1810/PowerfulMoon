#!/usr/bin/env python3
"""Check release identity and compare the packaged shader seed to the pinned upstream."""

import hashlib
import json
from pathlib import Path
import re
import subprocess
from urllib.request import urlopen


ROOT = Path(__file__).resolve().parents[2]
UPSTREAM = "Simon3Sarkis/super-odyssey-pc-recomp"
FORK = "yngvifreyr1810/PowerfulMoon"


def require(condition, error):
    if not condition:
        raise SystemExit(error)


def main():
    manifest = json.loads((ROOT / "release.json").read_text(encoding="utf-8"))
    version = manifest["version"]
    tag = manifest["upstream_tag"]
    commit = manifest["upstream_commit"]
    require(re.fullmatch(r"v\d+\.\d+\.\d+", tag), "Invalid upstream tag")
    require(re.fullmatch(r"[0-9a-f]{40}", commit), "Invalid upstream commit")
    require(re.fullmatch(re.escape(tag[1:]) + r"\.[1-9]\d*", version), "Invalid fork version")

    version_header = (ROOT / "runtime/version.h").read_text(encoding="utf-8")
    resources = (ROOT / "runtime/platform/windows.rc").read_text(encoding="utf-8")
    require(f'#define APP_VERSION "{version}"' in version_header, "APP_VERSION mismatch")
    require(f'#define APP_UPDATE_REPO "{FORK}"' in version_header, "Updater repo mismatch")
    for field in ("FileVersion", "ProductVersion"):
        require(f'VALUE "{field}", "{version}"' in resources, f"{field} mismatch")
    numeric = r"\s*,\s*".join(version.split("."))
    for field in ("FILEVERSION", "PRODUCTVERSION"):
        require(re.search(rf"(?m)^{field}\s+{numeric}\s*$", resources), f"{field} mismatch")

    ref = f"refs/tags/{tag}"
    listing = subprocess.check_output(
        ["git", "ls-remote", "--exit-code", f"https://github.com/{UPSTREAM}.git", ref, ref + "^{}"],
        text=True,
        timeout=30,
    )
    refs = {full_ref: sha for sha, full_ref in map(str.split, listing.splitlines())}
    require(refs.get(ref + "^{}", refs.get(ref)) == commit, f"{tag} does not point to {commit}")

    seed_url = f"https://raw.githubusercontent.com/{UPSTREAM}/{commit}/data/pipeline-seed.bin.zst"
    with urlopen(seed_url, timeout=30) as response:
        expected = hashlib.sha256(response.read()).hexdigest()
    actual = hashlib.sha256((ROOT / "data/pipeline-seed.bin.zst").read_bytes()).hexdigest()
    require(actual == expected, "The shader seed differs from the pinned upstream revision")
    print(f"PowerfulMoon {version} · upstream {tag} {commit} · seed SHA-256 {actual}")


if __name__ == "__main__":
    main()
