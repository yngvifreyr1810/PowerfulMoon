#!/bin/bash
# The launcher may ship the upstream pipeline seed. Reject player data and generated game caches.
set -euo pipefail
root=${1:?usage: dist_check_content.sh <package-directory>}
cd "$root"
manifest=$(find . -mindepth 1 -print)
if grep -Ei '^(\./|\./usr/bin/)(cache|save|saves|save-cap|extracted|exefs|romfs|mods|reports)(/|$)|(^|/)(graphics-pipelines\.bin|shader[-_]?cache[^/]*\.bin|odyssey_game\.[^/]+|prod\.keys|title\.keys|data-folder\.txt|powermoon\.ini|settings\.ini)$|\.(nsp|nsz|xci|xcz|nca|nro|nso|sav|bfsar)$' <<< "$manifest"; then
    echo 'dist: package contains local game data or cache files' >&2
    exit 1
fi
seed=./pipeline-seed.bin
if [[ -d ./usr/bin ]]; then seed=./usr/bin/pipeline-seed.bin; fi
if [[ ! -f "$seed" ]]; then
    echo 'dist: upstream pipeline seed is missing' >&2
    exit 1
fi
echo 'dist: upstream pipeline seed present, no game files, saves, or generated shader caches'
