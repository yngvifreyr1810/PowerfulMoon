#!/bin/bash
# usage: tools/dist_check_paths.sh <dist folder>: fails when a file of the release names the build machine's user folder
# (debug info, a tool's cache path, a log: "Users\<name>", "home/<name>" in any spelling). A build in a neutral folder
# (C:\PowerMoonRecomp) may name that folder; a user folder would name a person. Empty names are never searched for.
set -u
D="$1"
N="${USERNAME:-}"
if [ -z "$N" ]; then  # (MSYS2's make started from Git Bash passes no USERNAME: Windows' whoami says "machine\name")
    N="$(/c/Windows/System32/whoami.exe 2>/dev/null | tr -d '\r')"
    N="${N##*\\}"
fi
[ -n "$N" ] || { echo "tools/dist_check_paths.sh: no user name to check for"; exit 1; }
if grep -rlaiE "(users|home)[/\\]+$N([/\\]|$)" "$D"; then
    echo "tools/dist_check_paths.sh: the files above name this machine's user folder ($N)"
    exit 1
fi
echo "dist: no user folder inside"
