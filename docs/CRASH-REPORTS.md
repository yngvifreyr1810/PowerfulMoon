# Diagnostic and crash reports

F1 / Esc > Quit > **Copy crash report** creates one text file and copies its
**text** (not its path) to the clipboard: paste it straight into the issue.
**Show report file** opens its folder (Explorer with the file selected; on
Linux the folder) to attach the file instead. This action saves the current
settings before collecting them. Nothing is uploaded automatically.

## Reason first (format 3, 2026-11-19)

Every report starts with three lines in plain words:

```text
Crash reason: Out of graphics memory (VRAM)
What to try: Lower the internal resolution (Graphics), set Performance > Memory use to Low ...
Error: FATAL: vmaCreateImage(...) failed: -2 (...)
```

The reason comes from the first processor exception, the log's last `FATAL:` or
`*** ` line, and the PC's free memory at the crash (runtime/diagnostics.c
`classify`). Kinds: out of graphics memory (VRAM: Vulkan -2, the texture
message), out of memory (RAM: Vulkan -1, "out of host memory", or a crash while
free memory was nearly gone), Vulkan device lost (-4), a missing game file
(`cannot open` / `cannot load` / `romfs:` / exefs messages), another Vulkan
error, an illegal instruction (CPU features, with the processor's AVX2 / FMA /
BMI2 / AVX / SSE4.2 listed), any other FATAL message (`Error: <message>`), a
crash in the graphics driver, stack overflow, heap corruption, abort, or a
crash with no error line.

## Crash dialog and the next start

Windows: the error dialog leads with the reason and has **Copy report text**
and **Show report file** (the dialog comes back after each, until Close). A
report is saved before the dialog opens, even if the player chooses Close.

Linux / Steam Deck: the error box (no buttons: zenity can hang) shows the
reason and the report's path.

Both: at the next start the start page shows once, even when "Show this screen
at every start" is off, with a **THE GAME CRASHED LAST TIME** card: the reason,
**Copy report text**, **Show report file**, Dismiss. Scripted runs
(`ODYSSEY_NO_CONFIG=1`, `--play`) skip it. Later, **Copy last crash report** and
**Show last crash report file** stay in the Quit page.

Files are `report-<UTC>-<process>-<sequence>.txt` under `reports/` beside the
current file log. Without a file log, the game-data folder is used, or the
executable's folder before data initialization. `reports/.last-crash` (hidden)
remembers the last crash: the report's name, its reason, and `seen` once the
next start's card was shown. It used to be `last-crash.txt`, holding only the
report's name, and a player attached that instead of the report (#75); an
older `last-crash.txt` is still read, and removed at the next crash.

The copied text is markdown for an issue comment: the reason in bold, the report
folded in a `<details>` block. GitHub takes 65,536 characters in a comment, so a
longer report keeps its first part (reason, summary, PC, settings) and the log's
last ~40 KB, and says that the file has all of it.

## Forced crashes (tests)

`ODYSSEY_TEST_CRASH=<kind>` stops the game `ODYSSEY_TEST_CRASH_AFTER` seconds
(default 20) after start with the same error lines as the real crash: `vram`,
`ram`, `devlost`, `illegal` (a real illegal instruction), `fatal`, `segv`,
`ram-segv` (a real access violation with memory reported low,
`ODYSSEY_TEST_MEMORY_LOW`), `missing-file` (the RomFS image opened under a name
that isn't there, services/fs.c). `ODYSSEY_TEST_CRASH_DIALOG=copy|show|close`
presses that dialog button without showing it (the dialog's text goes to the
log). The game only writes a report when it logs to a file, i.e. started without
a console (e.g. PowerShell `Start-Process`).

The report contains:

- App version and a cached FNV-1a executable fingerprint for distinguishing
  private builds sharing a version number (not a security verification hash).
- OS/platform, CPU name where available, logical processors and total RAM.
- Selected Vulkan GPU, driver name/information, raw driver version, device IDs
  and API version, cached while the device is healthy.
- Loaded game main build ID, first recorded host exception/address/PC/LR and
  the fault/backtrace/guest call chain already written to the log.
- Saved settings (up to 64 KiB), startup log (64 KiB for oversized logs) and
  most recent log entries (up to 1 MiB).

Only the host executable is read for the startup fingerprint. Bundle contents
come from the settings and log, without collecting game assets, saves, a
directory archive or a process memory dump. The text can contain local paths
and mod names; review it before sharing. Terminal-only runs explicitly report
when no file log is available.

Fatal export is best-effort: memory/stack corruption, full or unwritable disks,
forced termination, or failures before logging/handlers initialize can prevent
capture. Original logging remains in place. Failed exports do not overwrite an
existing report. Missing GPU/game metadata is labeled unavailable rather than
queried from a failed device.

Windows clipboard ownership works before a game window exists. Other platforms
queue copying on SDL's event thread without blocking rendering; the menu shows
Copy pending until completion, following the requirements of
[SDL_SetClipboardText](https://wiki.libsdl.org/SDL3/SDL_SetClipboardText) and
[SDL_RunOnMainThread](https://wiki.libsdl.org/SDL3/SDL_RunOnMainThread).

## Validation

Windows error-dialog and in-game buttons were exercised; the saved report was
inspected for actual CPU/GPU/driver/OS/game-build, settings and log. Quit
confirmation and exit were exercised. Native fixtures cover early startup and
missing settings, bounded logs retaining startup and crash tail, first-fault
preservation, unique files, small buffers, unavailable output directories,
rejecting an outside-folder marker, a known executable fingerprint, and recovery
in a new process after the old log is replaced.

Linux/macOS still need native builds and runtime tests. macOS has no new fatal
popup; its existing fatal/terminal behavior is retained and the in-game action
is available. Metal GPU metadata is not supplied by the Vulkan cache hook.

Fixtures require no game data. In an MSYS2 UCRT64 development shell:

```sh
mkdir -p build/report-tests
clang -O2 -Iruntime tools/test_diagnostics.c runtime/diagnostics.c -lSDL3 -o build/report-tests/check.exe
```

Run with a **new** directory as the first argument and mode `fault`, `early`,
or `capture` as the second. Mode `restart` reuses the directory from `capture`
to check cross-process preservation. Other modes refuse existing directories.
Mode `dialog` opens a labeled test popup; build that interactive fixture as a
GUI executable and launch normally so its dialog is visible. Never use
production folders for fixtures.
