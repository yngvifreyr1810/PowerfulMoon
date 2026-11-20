#!/bin/bash
# Run on the Deck: the scripted Cap Kingdom run (deck-bench.sh, handheld settings unless USER_INI is given) and, from
# 62 s after the start, 20 s of the whole process's CPU time (perf stat) and which functions it went to (perf record).
# Prints: CPU ms per second of play, the frame rate in that window, the busiest threads, the top functions.
#   deck-cpu.sh <name> [GAME_ENV...]      e.g.  deck-cpu.sh base     deck-cpu.sh try1 ODYSSEY_X=1
# Results in ~/sopr/bench/<name>/ (cpu.txt, perf.txt). The game must not be running already.
n=${1:?name}; shift
ps -eo comm | grep -qi "[S]uperOdys" && { echo "a game is running"; exit 3; }
OUT=$HOME/sopr/bench/$n
[ -f /tmp/hh.ini ] || sed "s/^console_mode = .*/console_mode = handheld/; s/^aspect = .*/aspect = fill/" /run/media/deck/SN01T/sopr-fonttest/settings.ini > /tmp/hh.ini
ge="ODYSSEY_VK_PROF="; for e in "$@"; do ge="$ge,$e"; done
(OUT=$OUT APP=${APP:-/home/deck/sopr/build/linux/x86_64/AppDir/AppRun} FULL=1 USER_INI=${USER_INI:-/tmp/hh.ini} GAME_ENV=$ge SAMPLE_AT=999999 \
    INPUT="600:A:8,750:A:8,900:A:8,1050:A:8,2400:A:8,2600:A:8,2800:A:8" SHOT=5400 setsid nohup bash ~/sopr/tools/linux/deck/deck-bench.sh > /dev/null 2>&1 < /dev/null &)
sleep 62
pid=$(pgrep -x SuperOdysseyRec | head -1); [ -n "$pid" ] || { echo "the game did not start"; exit 1; }
perf stat -e task-clock -p $pid -o $OUT/cpu.txt -- sleep 10 > /dev/null 2>&1
perf record -F 499 -g -p $pid -o /tmp/perf-$n.data -- sleep 10 > /dev/null 2>&1
ms=$(grep task-clock $OUT/cpu.txt | awk '{gsub(",","",$1); print $1}')
L=/run/media/deck/SN01T/sopr-fonttest/SuperOdysseyRecomp.log
echo "$n: CPU $(python3 -c "print(round(float('$ms')/10))") ms per second of play ($(python3 -c "print(round(float('$ms')/10/166.7,1))") ms a frame at 60); fps $(grep -a "^\[vk\] t=" $L | tail -4 | awk '{printf "%s ", $5}'); $(tail -2 $OUT/gpu.log | head -1 | sed 's/.*power/power/')"
perf report -i /tmp/perf-$n.data --stdio --no-children --sort comm,dso -g none 2>/dev/null | grep -v "^#" | grep "%" | head -7 > $OUT/perf.txt
perf report -i /tmp/perf-$n.data --stdio --no-children --sort dso,symbol -g none 2>/dev/null | grep -v "^#" | grep -E "^ +[0-9]" | grep -v odyssey_game | head -22 | cut -c1-90 >> $OUT/perf.txt
cat $OUT/perf.txt
for i in $(seq 1 60); do pgrep -x SuperOdysseyRec > /dev/null || break; sleep 2; done
pkill -f "deck-[t]hreads.sh"; pkill -f "deck-[g]pu.sh"; true
