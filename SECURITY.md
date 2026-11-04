# What PowerfulMoon installs

Setup verifies every file against the embedded package's SHA-256 manifest before changing program files.
It installs only the listed app/dependency/source files into the folder you choose. The portable ZIP
contains the same files. Existing save/, extracted/, mods/, user fonts, powermoon.ini and the controller
profile are not packaged, deleted or overwritten. Replaced program files and copies of settings are
backed up in powerfulmoon-backups/. If a program-file copy fails, affected program files are restored.
Known old Power Moon EXEs/launchers are moved to backup only when their exact hashes match this release.

The installer checks that a game in the destination folder is closed and creates a Start-menu shortcut.
It requests no administrator rights for the host and registers no game startup entry or service.
PowerfulMoon starts the separate bundled ProCon2Bridge if no bridge is listening locally. That bridge's
first setup may request Windows approval to install the bundled signed USB/IP driver. Its behavior,
local ports, startup registration and removal are documented in controller-bridge/SECURITY.md.

Project executables are unsigned. Checksums provide integrity, not a promise of zero risk; keep your
antivirus enabled. The installer contains no remote script execution or telemetry. Upstream's optional
setup/compiler downloads remain upstream behavior; the full Windows package includes the setup tools.
The user's own game code is generated locally and must not be redistributed.

To remove the host: exit the game, preserve your saves/settings/game data, and remove the installed
program files listed in PACKAGE.json and the PowerfulMoon Start-menu shortcut. Backups remain available
for recovery. Remove the bridge separately according to its SECURITY.md; its shared USB/IP driver may
also be used by other applications. No game files, keys, firmware, Nintendo artwork or SDK are supplied.

Report issues without raw private logs or game content. The host log is PowerfulMoon.log; controller
logs can contain local Bluetooth device addresses. Credits and dependency source/licensing details are
in CREDITS.md, source/ and licenses/.

## Update channel

The updater contacts the public GitHub Releases API and downloads only published stable PowerfulMoon installers with matching repository URLs and SHA-256 metadata. Scheduled upstream integration creates review candidates, never app updates. It does not send game data, saves, controller profiles or device addresses. See [the approval workflow](docs/UPDATES.md).
