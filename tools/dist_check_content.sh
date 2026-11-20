#!/bin/bash
# The launcher may ship the upstream pipeline seed. Reject player data and generated game caches.
set -euo pipefail
root=${1:?usage: dist_check_content.sh <package-directory>}
source_seed="$(cd -- "$(dirname -- "$0")/.." && pwd)/data/pipeline-seed.bin.zst"
cd "$root"
manifest=$(find . -mindepth 1 -print)
if grep -Ei \
    -e '^(\./|\./usr/bin/)(cache|save|saves|save-cap|extracted|exefs|romfs|mods|reports)(/|$)' \
    -e '(^|/)(shaders\.pack|(graphics-pipelines|vk_pipelines|shader[-_]?cache[^/]*)\.bin)(\.[^/]+)?$' \
    -e '(^|/)(odyssey_game\.[^/]+|prod\.keys|title\.keys|data-folder\.txt|powermoon\.ini|settings\.ini|version\.dll)$' \
    -e '\.(nsp|nsz|xci|xcz|nca|nro|nso|sav|bfsar)$' <<< "$manifest"; then
    echo 'dist: package contains local game data or cache files' >&2
    exit 1
fi
seed=./pipeline-seed.bin.zst
if [[ -d ./usr/bin ]]; then seed=./usr/bin/pipeline-seed.bin.zst; fi
if [[ ! -f "$seed" ]] || [[ "$(sha256sum < "$source_seed")" != "$(sha256sum < "$seed")" ]]; then
    echo 'dist: pipeline seed is missing or differs from the build source' >&2
    exit 1
fi
echo 'dist: upstream pipeline seed present, no game files, saves, or generated shader caches'
