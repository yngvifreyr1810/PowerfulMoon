# Contributing to Power Moon Recomp

Bug reports, testing on different hardware and code contributions are welcome.

## The one hard rule: no Nintendo files

Nothing owned by Nintendo may ever be committed or attached to an issue or pull request:

- no copies of the game in any form, no keys
- no extracted game files (`extracted/`), no generated code (`generated/`), no game code library
  (`odyssey_game.dll`), no fonts (`.bfttf` / Switch `.ttf`)
- no game assets of any kind: textures, models, music, text, level files - also not "just one file for testing"

`.githooks/pre-commit` refuses such files (`git config core.hooksPath .githooks`). Screenshots and short clips of the
game in bug reports are fine.

## Reporting bugs

Please include your hardware (CPU, GPU, driver version), what you did, what happened and what you expected, and the
log: `PowerMoonRecomp.log` next to `PowerMoonRecomp.exe` (it names the crash and the game's call chain). A save from just before the problem
helps a lot.

## Code

- Match the style of the surrounding code; comments say why, not what.
- The game is the reference: a fix should make the port behave like the Switch, not hide a symptom. The debugging
  tools are environment variables (`ODYSSEY_*`), documented where they are read.
- Test a change in the game before sending it (`tools/odyssey_run.sh`, a stage warp: `ODYSSEY_WARP`).
