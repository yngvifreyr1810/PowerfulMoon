# Brazilian Portuguese fan translation

Super Mario Odyssey has no native Brazilian Portuguese language slot. The
**Português (Brasil)** setting uses the **US English (`USen`)** slot with the
separately installed **JumpManClub Brasil v1.1** RomFS translation.
This setting changes the game's language over the save's language at startup;
it does not translate the PC port's own F1 menu.

The F1 menu and the configuration screen have their own language, chosen in
**F1 > Language > Language** (the same languages as the game's own setting; only Korean and
**Português (Brasil)** have a translation so far, the others show the menu in English). It
is independent of the game's text language; with Auto, the menu follows the game's
language when it has a translation for it, so picking Português (Brasil) for the game
also puts the menu in Portuguese. It applies at the next start. The translation is
`runtime/ui/lang/pt-BR.txt`.

## Obtain and install

In **Controls > System language**, select **Português (Brasil)**. If the
translation is absent, confirm **Instalar tradução brasileira?**, wait for the
download and verification, then choose **Reiniciar agora** (save your progress
first) or restart later. On the next start the game uses Portuguese, including
with an existing save. An already recognized disabled RomFS package is enabled
without downloading it again. Conflicting mods are left unchanged and reported.

The installer downloads directly from the authors' official Google Drive link
after confirmation. It accepts only the pinned Atmosphere v1.1 archive
(773,795 bytes, SHA-256
`c282c43894796cb629f8668b2aec3fee50ddad88fcb7a92a5fc63f570c4409d3`).
It checks the archive before extraction and all four extracted files before
publishing the mod. HTML sign-in pages, modified downloads and revised packages
are rejected. Downloads are not mirrored or bundled with releases.

Installation uses a private temporary directory inside `mods/` and an atomic
rename to `mods/JumpManClub Brasil`. Existing destination folders, including
empty ones, are never replaced. Cancellation or a failed attempt removes that
attempt's temporary files; forced termination may leave a hidden
`.ptbr-install-*` directory, which the mod loader ignores. After closing the
application, that leftover directory can be removed manually.

Windows uses its system `tar.exe`. Linux AppImages bundle `bsdtar` from
libarchive; source installations need `libarchive-tools` (and `curl` for the
existing download transport). Neither the download nor extraction blocks the
menu thread. Internet access and a writable game-data folder are required.

### Manual installation

Obtain the **Atmosphere** package from the
[JumpManClub Brasil translation page](https://www.jumpmanclubbrasil.com.br/post/mario-odyssey).
Do not download or install a copy of the game from a translation mirror.
Extract the archive and copy its `romfs` folder into the **game-data folder**:

```text
mods/
  JumpManClub Brasil/
    romfs/
      LocalizedData/
        USen/
          LayoutData/
            FontData.szs
          MessageData/
            LayoutMessage.szs
            StageMessage.szs
            SystemMessage.szs
```

Keeping the package's Atmosphere directories also works:
`mods/JumpManClub Brasil/atmosphere/contents/0100000000010000/romfs/...`.
Use F1 > Mods > Open mods folder to locate the correct folder, including on
Linux installations with a separate data directory. Leave the original game
files and saves in place. The supplied font archive is required for accents.

Restart the application after installing files. In **Controls > System language**,
select **Português (Brasil)** and ensure the translation is enabled in **Mods**.
The description reports detection, disabled files, incomplete/modified packages,
unsupported code patches, or a conflicting mod. Restart again if the game was
already running. The preference is saved as `language = pt-BR` in `powermoon.ini`.

## Detection and limitations

Detection checks the sizes and SHA-256 fingerprints of all four files in the
official Atmosphere v1.1 package. A folder name alone never identifies a
translation. Fingerprints are checked once during the existing mod scan, with
at most 580 KB of temporary file data; no checks run per frame. Detection uses
the same case-insensitive paths and alphabetical priority as the RomFS loader.
It reports the package's identity and effective mod priority, not a guarantee
that every message or scene works with every game version.

The inspected package contains three Yaz0/SARC message archives with MSBT files
and one font archive, without ExeFS patches. Its archive entry names cover those
in the supplied 1.4.1 files. Other distributions or revised translation assets
must be inspected before their fingerprints can be recognized. Generic RomFS
mods continue to load as before, even when not recognized by this setting.

There is no native `pt-BR` game code: system-language queries and the existing
save-language override receive `en-US` / `USen`. If the translation is missing,
the preference remains saved and the description explains that the English slot
is being used. Auto retains the save's language as before; it does not detect
Portuguese from the host locale.

Game 1.0.0, Linux gameplay and full-game translation coverage require separate
testing. Do not infer compatibility from the translation's version number or
from successful file detection. The current validation record accompanies the
contribution; unexecuted checks must remain explicitly unverified.

## Current validation

The follow-up installer was built against the merged PT-BR support at
`7cf0405a590f76c7d1fcb6bef5ed1b62a2202c9d` on Windows 11 x64 with game 1.4.1.
All 13 installer tests passed with the private official archive and the opt-in
real network check. Without private assets or network access, eight passed and
five were skipped. Tests cover exact-package installation, spaces and Unicode
paths, malformed/modified/oversized packages, cancellation, existing destination
preservation, symlink refusal, and preservation of synthetic saves/settings/mods.

Bounded native game runs verified the missing-package prompt, keyboard
confirmation, official download, completion prompt, saved `pt-BR` preference,
and the actual Restart now action. The restarted process produced a Portuguese
title-screen capture. Another run selected PT-BR from Auto and verified the
existing-destination error while retaining Auto and the existing directory.
The original production save hashes remained unchanged. The 13 localization,
four camera-settings, eight Steam-shortcut and one generated-wrapper tests,
the gameplay-clock fixture and the crypto known-answer checks also passed.

Linux packaging scripts passed shell syntax checks. The new AppImage build,
Linux installer execution, game 1.0.0, gamepad/mouse interaction, network failure
UI, long sessions and a controlled performance comparison were not executed.
The records below describe earlier localization validation, separately from
the installer checks.

Initial 0.2.10 validation on Windows 11 x64, NVIDIA RTX 5060 Ti, game 1.4.1:
the original and changed source builds completed. Bounded runs exited successfully with inspected screenshots:
original English, missing translation, installed PT-BR, the actual Controls
menu/detection message, the opening, an existing save copied into a private
folder, original English restored with the mod disabled, Japanese with the mod
still installed, and PT-BR restored on restart. Cap Kingdom's tutorial text
rendered in Portuguese. Original save hashes remained unchanged.

All 13 localization tests passed with the separately obtained package. Without
private assets, six tests passed and seven asset-dependent tests were skipped.
The existing camera-settings (4), Steam shortcut (8), generated-wrapper (1) and
AES/SHA-256 known-answer checks passed. During that initial validation, the existing standalone Vulkan pipeline
test failed to link due to missing `vk_device_fault_report` and `smaa_hlsl_src`
references in its harness; it was not changed as part of this issue. Actual
Vulkan game execution succeeded.

The contribution was also integrated with upstream **0.2.11**
(`6548def577d04f5ebea7964ceca894b8bb970a42`). The two integration conflicts
in settings.h and menu.mm were resolved while preserving the new menu-language
setting and the PT-BR game preference. The 0.2.11 original runtime and the
integrated runtime/executable built successfully; both reached inspected menu
captures. The localization tests now also verify independent persistence of
PT-BR/Japanese/English game preferences with Auto/English/Korean menu preferences.
Camera settings, Steam shortcuts, generated wrappers and the new upstream
gameplay-clock fixture passed on this base. The generator and game-library ID
were unchanged, so the existing locally generated game library was reused.

This is bounded 1.4.1 validation, not a complete playthrough. Individual visual
checks for every accented character, all dialogues, long sessions and a
controlled performance comparison are still pending. The translation's message
archives contain the requested accented characters; that is distinct from
visually checking each glyph. No 1.0.0 game files were available. Linux gameplay
and compilation have not been verified in this contribution.

## Troubleshooting and removal

- **English text:** verify the four files are under `romfs/LocalizedData/USen`,
  enable the mod, select Português (Brasil), and restart. Existing saves require
  an explicit language choice rather than Auto.
- **Unrecognized or incomplete:** obtain the original v1.1 Atmosphere package
  again and include `FontData.szs`. Do not mix packages or game files into it.
- **Conflicts:** mods higher in the alphabetical Mods list win for each file.
  Disable a conflicting text/font mod and restart. Other non-conflicting mods
  remain enabled; this feature never rewrites their configuration.
- **Missing accents or broken text:** check the font archive and conflicts,
  then report the game version, port version, log and a screenshot. Package
  detection alone does not establish correct font rendering.
- **Return to original English:** disable this mod in Mods, select English and
  restart. Selecting English alone still uses translated USen files while the
  RomFS mod is enabled. Other original languages can be selected normally.
- **Uninstall:** close the application, remove only this translation's mod
  directory, select an original language, and restart. Saves, original RomFS
  files and other mods need no removal.

## Credits and redistribution

Translation: **JumpManClub Brasil** (the included tutorial also credits Red Wolf
Traduções). Obtain their files separately. No redistribution licence was found
in the package, translation page or copyright page. Neither the translation's
assets nor Nintendo assets are included here. Do not commit these archives,
fonts, extracted MSBT files, generated game code or game libraries. The runtime
downloads the package only after the user confirms, directly from the authors'
distribution. It does not alter the original game files. No specific permission
for third-party hosting or automated downloading was found on the inspected
pages; public availability should not be described as a redistribution licence.

## Automated checks

From the build environment, with SDL3 installed:

```sh
clang -O2 -D_FILE_OFFSET_BITS=64 -Iruntime tools/test_localization.c \
  runtime/settings.c runtime/mods.c runtime/localization.c runtime/nxcrypto.c \
  -lSDL3 -o build/test_localization
tools/venv/Scripts/python.exe tools/test_localization.py build/test_localization.exe
```

On Linux, use `tools/venv/bin/python` and `build/test_localization` instead.
Optional private package checks use `PTBR_TEST_ROMFS=/path/to/translation/romfs`.
Without it, asset-dependent checks are skipped; no proprietary test fixtures
are supplied. All checks use temporary settings/mod directories and new
processes to verify persistence across restarts.

The installer fixture links the production runtime (without starting the game)
and needs no proprietary game files:

```sh
make -j16 test-ptbr-installer
```

Set `PTBR_TEST_ARCHIVE=/path/to/official-atmosphere.rar` for the private package
cases. `PTBR_TEST_NETWORK=1` separately opts into a real official download.
Neither is enabled by default. Keep the package outside Git.
