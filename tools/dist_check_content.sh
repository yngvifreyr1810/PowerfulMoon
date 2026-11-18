#!/bin/bash
# The launcher distribution must not contain the player's data or compiled game caches.
set -euo pipefail
root=${1:?usage: dist_check_content.sh <package-directory>}
cd "$root"
manifest=$(find . -mindepth 1 -print)
if grep -Ei '^(\./|\./usr/bin/)(cache|save|saves|save-cap|extracted|exefs|romfs|mods|reports)(/|$)|(^|/)(pipeline-seed\.bin|graphics-pipelines\.bin|shader[-_]?cache[^/]*\.bin|odyssey_game\.[^/]+|prod\.keys|title\.keys|data-folder\.txt|powermoon\.ini|settings\.ini)$|\.(nsp|nsz|xci|xcz|nca|nro|nso|sav|bfsar)$' <<< "$manifest"; then
    echo 'dist: package contains local game data or cache files' >&2
    exit 1
fi
echo 'dist: launcher and local build tools only (no game files, saves, or shader caches)'
