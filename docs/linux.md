# Power Moon Recomp on Linux (x86-64) - test builds

Status: playable (tested). The Linux build is made with `make appimage` (`tools/linux/appimage.sh`, in Ubuntu 22.04
containers) and published with the releases; reports welcome.

## Running the AppImage
    chmod +x "Power Moon Recomp-<version>-linux-x86_64.AppImage"
    ./"Power Moon Recomp-<version>-linux-x86_64.AppImage"

Needs: a distribution with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36, Arch, SteamOS, ...), a Vulkan 1.3
driver (Mesa RADV / ANV / NVK, or NVIDIA's), a CPU with AVX2 (2013 or newer), about 7 GB free space, and your own copy
of Super Mario Odyssey 1.0.0 (extracted: `exefs` and `romfs`). The setup page asks for that folder on the first start
and builds the game code on your PC (a few minutes, once).

Everything the app writes lives in `~/.local/share/power-moon-recomp` (`$XDG_DATA_HOME/power-moon-recomp`): game files
(`extracted/`), saves, settings (`powermoon.ini`), shader caches. Started without a terminal the log is
`~/.local/share/power-moon-recomp/PowerMoonRecomp.log` (from a terminal it prints there instead).
`ODYSSEY_DATA_ROOT=<dir>` uses another folder.

If the AppImage won't start with a FUSE error, install your distribution's `fuse3` package, or run it with
`APPIMAGE_EXTRACT_AND_RUN=1`. On a Steam Deck, put `APPIMAGE_EXTRACT_AND_RUN=1 %command%` in the game's launch options.

## Reporting a problem
Open an issue with your distribution, GPU and driver version, what happened, and the log file. Never attach game files.
