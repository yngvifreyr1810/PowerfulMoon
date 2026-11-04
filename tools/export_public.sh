#!/bin/bash
# usage: tools/export_public.sh [<public checkout>]: copies the files of the public repository - the tracked files of
# this one minus the internal development log (CLAUDE.md) and Dear ImGui's examples, docs and unused backends - into
# the public checkout (default C:\PowerMoonRecomp: a neutral path, so release builds made there name no user folder).
# Then, there: review `git status`, commit (its own identity: the public account's noreply address), push.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${1:-/c/PowerMoonRecomp}"
mkdir -p "$OUT"
keep_imgui='^third_party/imgui/(LICENSE\.txt|imconfig\.h|imgui\.cpp|imgui\.h|imgui_draw\.cpp|imgui_internal\.h|imgui_tables\.cpp|imgui_widgets\.cpp|imstb_rectpack\.h|imstb_textedit\.h|imstb_truetype\.h|backends/imgui_impl_(sdl3|vulkan|metal)\.(h|cpp|mm)|misc/fonts/Roboto-(Medium\.ttf|LICENSE\.txt))$'
n=0
while IFS= read -r f; do
    case "$f" in
        CLAUDE.md) continue ;;
        third_party/imgui/*) echo "$f" | grep -qE "$keep_imgui" || continue ;;
    esac
    mkdir -p "$OUT/$(dirname "$f")"
    cp -p "$f" "$OUT/$f"
    n=$((n + 1))
done < <(git ls-files)
echo "exported $n files to $OUT"
