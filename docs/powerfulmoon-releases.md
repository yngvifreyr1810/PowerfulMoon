# PowerfulMoon upstream maintenance

`powerfulmoon` starts at the published `v0.4.2.1` tag. The original `main` remains available for historical releases and patches. The launcher uses releases from `yngvifreyr1810/PowerfulMoon`. Only the owner-approved release process changes the public release.

## Review a new upstream tag

The Upstream release watch workflow runs every two hours. A manual run previews notices by default. Enable **Open an issue for each newly detected upstream release** to deliver missing notices. Each notice records the upstream release, its commit and the latest published PowerfulMoon baseline. Use the API's draft and prerelease flags for eligibility. The upstream release title can contain “preview” even when GitHub marks the release as stable.

Read the upstream changelogs and the complete diff from the recorded upstream commit through the proposed SHA, including releases skipped between the two. Record the touched fork features, risk and required checks in the review issue or PR:

- Vulkan display rate selection and VRR
- Switch controller orientation, gesture filtering, the shake binding and vibration gain
- Windows Streamline and optional DLSS Frame Generation setup
- Launcher version, updater repository and artifact names
- Windows and Linux packaging, upstream pipeline seed and exclusion of generated data

Use a candidate branch under `ci/upstream-vX.Y.Z` after the owner picks the update. For a compatible change, start from `powerfulmoon` and merge the pinned upstream SHA with a merge commit. For a renderer, input, build or data-format change, build the pinned upstream checkout first and port each applicable change from the published fork's net diff. Record any patch superseded by upstream. Preserve the notification workflow, release metadata and package validation. When the clean port is ready, start a candidate at `powerfulmoon`, start a non-fast-forward merge of the pinned upstream SHA and resolve the index to the verified clean port tree. Commit the merge and verify both parents and exact tree equality with the clean port. Do this in an isolated candidate checkout. If the change cannot be integrated safely, record the reason in the review and leave the published tag in place.

## Validate the exact release revision

Use GitHub's **Create a merge commit** for an approved PR. Never squash away the upstream parent. Update `release.json` in the candidate along with `APP_VERSION` and both Windows version resources. Use `X.Y.Z.1` for the initial fork release on upstream `vX.Y.Z` and increment the fourth number for follow-ups. CI verifies the version, pinned upstream tag, shader seed bytes, and package paths.

After the merge, record the build run whose `headSha` matches the final commit on `powerfulmoon`. Download its `windows-zip` and `linux-appimage` artifacts. Use copies of player data for testing. For Windows run clean setup, reach gameplay, load a copied save and restart. Exercise the features touched by the update. VRR should follow variable presentation, the tested DLSS configuration is 60 FPS with 2× FG on supported hardware, and Switch motion should distinguish preparation from the actual flick. Check the shake binding and 200% vibration setting with a controller. If upstream changes caches, test an empty cache and a separate copy of the previous cache. Confirm migration and a second launch.

For v0.4.3, verify zstd shader pack creation or migration, the NVIDIA pipeline cache path and the mouse binding used for shake. Compare the upstream and fork runs on the same test data, with independent cache and save copies. Keep the optional community FG proxy outside the distributable files.

## Stage and publish

1. For a verified candidate, record the build run, commit, smoke results and the two asset SHA-256 hashes. Use Windows ZIP from that run. Use the AppImage from the same run if Linux testing has passed. For shader or renderer updates without Linux gameplay results, keep the AppImage on Actions and publish Windows first. Preserve the exact AppImage and SHA-256 before the artifact expires.
2. On the fork create a draft using `gh release create vX.Y.Z.N <zip> -R yngvifreyr1810/PowerfulMoon --target <tested-sha> --draft --notes-file <notes>`. Release notes identify the upstream tag, fork changes and any platform still pending. Compare the draft asset digests from `gh release view --json assets` with the downloaded files.
3. After owner approval, use `gh release edit vX.Y.Z.N -R yngvifreyr1810/PowerfulMoon --draft=false --latest`. Verify the tag points to the tested SHA, the GitHub latest API returns the new tag and the Windows asset ends in `-windows.zip`. Previous releases remain available for rollback.
4. When Linux gameplay passes, upload the same verified AppImage to the same tag and recheck its published digest. If any check fails, keep the published release and Latest pointer at the prior known-good version.

The build packages the launcher, runtime and local generator. Keep the upstream `pipeline-seed.bin` intact. Omit save data, game images and libraries, keys, player settings and generated shader cache files such as `shaders.pack` from the packages and source archive. `tools/release.sh` is the inherited upstream publisher and is disabled for this fork.
