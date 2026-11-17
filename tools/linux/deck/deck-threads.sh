#!/bin/bash
# Every 2 s: each thread of the game's process with its CPU use (% of one core) over those 2 s, the busiest first.
OUT=$1
while ! pid=$(pgrep -f "SuperOdysseyRecomp$|/SuperOdysseyRecomp " | head -1) || [ -z "$pid" ]; do sleep 1; done
declare -A prev
while kill -0 $pid 2>/dev/null; do
    declare -A cur; line="$(date +%s)"
    for t in /proc/$pid/task/*; do
        tid=${t##*/}; s=$(cat $t/stat 2>/dev/null) || continue
        name=$(cat $t/comm 2>/dev/null); rest=${s##*) }; set -- $rest
        cur[$tid]=$(( ${12} + ${13} ))
        [ -n "${prev[$tid]}" ] && d=$(( (cur[$tid] - prev[$tid]) / 2 )) && [ $d -ge 5 ] && line="$line $name/$tid:$d%"
    done
    echo "$line" | tr ' ' '\n' | sort -t: -k2 -rn | tr '\n' ' ' >> $OUT; echo >> $OUT
    for k in "${!cur[@]}"; do prev[$k]=${cur[$k]}; done; unset cur
    sleep 2
done
