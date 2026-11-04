# Updates and upstream integration

[Huong dan tung buoc bang tieng Viet](UPDATES-VI.md).

PowerfulMoon follows [Power Moon Recomp](https://gitlab.com/zzzarathustra-dev/power-moon-recomp) while keeping its controller integration and presentation settings. Updating with an upstream Power Moon ZIP replaces the customized host; use a **PowerfulMoon release** instead.

## Update from the app

Open **F1 → Controls → Check for updates**. Startup checks can be disabled beside that button. Checks run at most once a day, stay quiet when offline, and never install anything automatically.

The updater offers only published stable releases from this repository. Downloading an installer does not replace the running game. Save and close the game, then choose **Install update**. The existing game folder is selected automatically; saves, mods, controller mappings and settings stay there. Replaced program files are backed up under `powerfulmoon-backups/`.

An internet connection and access to GitHub are required. SHA-256 verification detects a damaged or mismatched download; the community installer is not code-signed. See [SECURITY.md](../SECURITY.md).

## Independent upstream bot

**Upstream candidate bot** runs on GitHub Actions every two hours, or through **Actions → Run workflow**. It uses Python, Git and GitHub's temporary workflow token. It needs no Codex session, AI service, API key or running personal computer. GitHub may delay scheduled jobs and may disable schedules after prolonged repository inactivity.

1. Read the upstream stable release list and pin the exact source commit.
2. Apply the source delta in an isolated checkout using Git's three-way merge. Retain fork edits when Git can merge them. Upstream cannot replace this project's workflow, installer or updater scripts.
3. If conflicts or legal/dependency changes require attention, stop and open/update a review issue. No guessed conflict resolutions.
4. Otherwise push an `automation/upstream-v…` candidate branch, open a review issue and request **Build review candidate**. This Windows build downloads checksum-pinned public dependencies, runs tests and content checks, and uploads a complete installer/portable ZIP as an Actions artifact for 30 days.

The source candidate is public, but it is **not a release and is never offered by the app**. Build success cannot prove gameplay compatibility. Test motion, vibration, VRR/frame interpolation and an upgrade using existing saves/settings before approval. New dependency requirements can stop the build until the pinned inputs are reviewed.

## Owner approval and publication

After testing, the repository owner runs **Publish owner-reviewed candidate** from `main`, supplies the successful candidate run ID and exact tested commit, and types `PUBLISH`. The `release-approval` GitHub environment requires owner approval before the publish job starts. Keep that environment protection configured in repository settings.

Publication integrates the approved source into `main` so the next upstream merge starts from the accepted fork. If that source conflicts with newer `main` changes, publication stops for review. It then promotes the existing immutable artifact after verifying its source identity, content-audit report and checksums. It does not rebuild newer source. Assets upload to a draft first; only a complete release becomes public. Neither a schedule nor the source-sync bot can invoke this publication path as the owner.

To repair a conflict, inspect the review issue, merge upstream changes manually, and run the candidate build for that repaired commit. To retry an expired artifact, rebuild and test the new artifact before approving it. Each completed build posts its result and artifact link to the review issue; GitHub's notification settings control email delivery.

## Local use

`python tools/automation/upstream_sync.py --dry-run` prepares a candidate in a temporary checkout without pushing, opening issues or publishing. `python -m unittest discover -s tests -p 'test_*.py' -v` exercises merge conflicts, updater trust rules and transactional installation without game files.

The cloud build uses `tools/automation/bootstrap_windows.py` and `tools/windows/build_full.ps1`. It reuses only allowlisted redistributables from a pinned approved baseline; no game folder, generated game library, save, key or Nintendo asset is needed or uploaded.
