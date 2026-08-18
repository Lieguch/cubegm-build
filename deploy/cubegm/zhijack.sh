#!/bin/sh
# =============================================================================
#  zhijack.sh -- RK3036G (ARM, HDMI 1280x720, standard Linux DRM/ALSA/evdev)
# =============================================================================
#  Reached via the stock boot chain:
#    rkgame (untouched) -> setting.xml autorun
#      <autorun file="/mnt/sdcard/MD/dummy.md" driver="" />
#      driver="" -> rkgame resolves core by ROM ext (.md) -> dlopens
#      cubegm/cores/libemu_md.so = our libemu_tfhijack.so
#        retro_load_game() forks THIS script.
#
#  rkgame stays ALIVE (kept in game-mode by tfhijack's idle retro_run), so the
#  stock input pipeline is preserved; this script launches picoarch + FrogUI,
#  which own the display (standard DRM/KMS) and read input via evdev (RK3036G
#  has NO cubevol — driver.so strings confirm standard Linux input).
#
#  SAFETY: this script never touches root.dat or any checksum/partition data.
# =============================================================================
mkdir /tmp/zhijack.lock 2>/dev/null || exit 0

# --- opt-in diagnostics (don't chew the SD in normal use) --------------------
if [ -f /mnt/sdcard/log.txt ]; then
    LOG=/mnt/sdcard/log.txt
    mv "$LOG" "$LOG.prev" 2>/dev/null
    : > "$LOG"
    echo "=== zhijack boot [rk3036g] $(date '+%H:%M:%S' 2>/dev/null) ===" >> "$LOG"
    sync
else
    LOG=/dev/null
fi

# Freeze icube (the respawner) THEN kill rkgame so the stock menu can't redraw
# over our frames and nothing respawns a fresh rkgame.
kill -STOP $(pidof icube) 2>/dev/null
killall rkgame 2>/dev/null
echo "icube frozen, rkgame killed" >> "$LOG"

# Device environment for picoarch / frontends that read /tmp/tfdevice.env.
cat > /tmp/tfdevice.env <<EOF
TF_DEVICE=rk3036g
TF_PANEL_W=1280
TF_PANEL_H=720
TF_UI_SCALE=150
TF_ASPECT_NUM=16
TF_ASPECT_DEN=9
TF_ROTATE=0
TF_PRESENT=1
TF_DRIVER=
EOF
export TF_DEVICE=rk3036g TF_PANEL_W=1280 TF_PANEL_H=720 TF_UI_SCALE=150

# Runtime libs the device rootfs does NOT provide (SDL/libpng12/z) ship in
# cubegm/lib; picoarch's NEEDED entries resolve against it.
export LD_LIBRARY_PATH=/mnt/sdcard/cubegm/lib:/mnt/sdcard/cubegm/usr/lib:$LD_LIBRARY_PATH

# CPU: force max-performance governor (helps every emulator).
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -w "$g" ] && echo performance > "$g" 2>/dev/null
done
for c in /sys/devices/system/cpu/cpu*/cpufreq; do
    mx=$(cat "$c/cpuinfo_max_freq" 2>/dev/null)
    [ -n "$mx" ] && [ -w "$c/scaling_min_freq" ] && echo "$mx" > "$c/scaling_min_freq" 2>/dev/null
done

# RK3036G input is standard evdev — picoarch (plat_linux.c) reads it directly.
# No cubevol/gpio shim is required (driver.so has no cubevol references).

PICOARCH=/mnt/sdcard/cubegm/picoarch
FROGUI_CORE=/mnt/sdcard/cubegm/cores/frogui_libretro.so
LAUNCH=/tmp/frogui_launch.txt

# Front-end launcher loop. FrogUI writes $LAUNCH (core path on line 1, ROM path
# on line 2) when the user picks a game; we then launch picoarch with that core.
ITER=0
while true; do
    ITER=$((ITER+1))
    rm -f "$LAUNCH"
    killall rkgame 2>/dev/null
    echo "--- iter $ITER: frogui ---" >> "$LOG"
    "$PICOARCH" "$FROGUI_CORE" "$FROGUI_CORE" >> "$LOG" 2>&1
    RC=$?
    echo "frogui exited rc=$RC" >> "$LOG"
    if [ -f "$LAUNCH" ]; then
        CORE_PATH=$(sed -n '1p' "$LAUNCH")
        ROM_PATH=$(sed -n '2p' "$LAUNCH")
        rm -f "$LAUNCH"
        if [ -n "$CORE_PATH" ] && [ -n "$ROM_PATH" ]; then
            killall rkgame 2>/dev/null
            sleep 0.3
            echo "--- iter $ITER: game [$CORE_PATH] via $PICOARCH ---" >> "$LOG"
            "$PICOARCH" "$CORE_PATH" "$ROM_PATH" >> "$LOG" 2>&1
            GRC=$?
            echo "game exited rc=$GRC" >> "$LOG"
        fi
    fi
    sleep 0.2
done
