#!/bin/bash
# GPU busy % and clock, 10 times a second, averaged over each 2 s: what the GPU did while the game ran
OUT=$1
while ! pgrep -f "SuperOdysseyRecomp$|/SuperOdysseyRecomp " >/dev/null; do sleep 1; done
while pgrep -f "SuperOdysseyRecomp$|/SuperOdysseyRecomp " >/dev/null; do
    s=0; n=0; clk=""
    for i in $(seq 1 20); do b=$(cat /sys/class/drm/card0/device/gpu_busy_percent); s=$((s+b)); n=$((n+1)); sleep 0.1; done
    clk=$(grep "\*" /sys/class/drm/card0/device/pp_dpm_sclk | head -1)
    echo "$(date +%s) busy $((s/n))% clock $clk power $(( $(cat /sys/class/hwmon/hwmon7/power1_input 2>/dev/null || echo 0) / 1000000 ))W" >> $OUT
done
