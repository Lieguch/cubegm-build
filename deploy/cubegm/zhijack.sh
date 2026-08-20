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
#
#  v2 (2026-08-20): observability build.
#    - LOG is ALWAYS /mnt/sdcard/zhijack.log (no more opt-in /dev/null),
#      and picoarch's stderr (fb0-direct status etc.) lands in it too.
#    - Before picoarch starts, a full-white test frame is written to /dev/fb0:
#        white flash visible -> fb0 -> HDMI link is good, issue is inside
#                                 picoarch (read zhijack.log / picoarch_init.log)
#        stays black          -> fb0/HDMI link itself is the problem
# =============================================================================
mkdir /tmp/zhijack.lock 2>/dev/null || exit 0

# --- diagnostics: ALWAYS write a boot log (cheap; survives power-cycle) -------
LOG=/mnt/sdcard/zhijack.log
: > "$LOG"
echo "=== zhijack boot [rk3036g] $(date '+%H:%M:%S' 2>/dev/null) ===" >> "$LOG"
echo "pid=$$ cmdline=$(cat /proc/$$/cmdline 2>/dev/null)" >> "$LOG"
sync

# --- boot-chain marker: did the stock launcher really reach us? ---------------
echo "zhijack: reached, uname=$(uname -a 2>/dev/null)" >> "$LOG"

# Freeze icube (the respawner) THEN kill rkgame so the stock menu can't redraw
# over our frames and nothing respawns a fresh rkgame.
kill -STOP $(pidof icube) 2>/dev/null
killall rkgame 2>/dev/null
echo "icube frozen, rkgame killed (pidof icube=$(pidof icube 2>/dev/null))" >> "$LOG"

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

# --- fb0 link test: full-white frame (RGB565 0xFFFF / 32-bit 0xFFFFFFFF) -------
echo "zhijack: fb0 white-frame test..." >> "$LOG"
if [ -e /dev/fb0 ]; then
    head -c 1843200 /dev/zero 2>/dev/null | tr '\000' '\377' > /dev/fb0 2>>"$LOG"
    echo "zhijack: fb0 white frame rc=$? (white flash visible = fb0->HDMI OK)" >> "$LOG"
else
    echo "zhijack: /dev/fb0 NOT PRESENT -> fb0 path cannot work" >> "$LOG"
fi
ls -la /dev/fb* /dev/dri 2>/dev/null >> "$LOG"

# --- DRM/KMS + fb0 topology dump (payload-237 showed fb0 writes OK but HDMI
#     stays black -> is fb0 really the HDMI source? RK3036 is DRM/KMS.) --------
echo "=== DRM sysfs ===" >> "$LOG"
ls /sys/class/drm/ 2>/dev/null >> "$LOG"
for c in /sys/class/drm/card0-*; do
    [ -e "$c" ] || continue
    s=$(cat "$c/status" 2>/dev/null); e=$(cat "$c/enabled" 2>/dev/null)
    m=$(cat "$c/modes" 2>/dev/null | tr '\n' ' ')
    echo "DRM $c status=$s enabled=$e modes=[$m]" >> "$LOG"
done
echo "=== fb0 sysfs ===" >> "$LOG"
cat /sys/class/graphics/fb0/name 2>/dev/null >> "$LOG"
cat /sys/class/graphics/fb0/virtual_size 2>/dev/null >> "$LOG"
cat /sys/class/graphics/fb0/stride 2>/dev/null >> "$LOG"
cat /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null >> "$LOG"
echo "=== DRM master / process ===" >> "$LOG"
cat /sys/class/drm/card0/device/driver/uevent 2>/dev/null | head -5 >> "$LOG"
echo "=== ALSA cards ===" >> "$LOG"
cat /proc/asound/cards 2>/dev/null >> "$LOG"
ls /proc/asound/ 2>/dev/null | head -20 >> "$LOG"
echo "=== input devices (gamepad keycodes) ===" >> "$LOG"
cat /proc/bus/input/devices 2>/dev/null >> "$LOG"
ls -la /dev/input/ 2>/dev/null >> "$LOG"
sync
sleep 0.3

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
    # icube is the respawner: if it revives (crash-restart), it respawns
    # rkgame which takes DRM master back -> HDMI switches away from our dumb
    # buffer (observed as half-white after ~10 min). Kill it every iteration.
    killall icube 2>/dev/null
    killall rkgame 2>/dev/null
    echo "--- iter $ITER: frogui (icube=$(pidof icube 2>/dev/null)) ---" >> "$LOG"
    if [ ! -x "$PICOARCH" ]; then
        echo "zhijack: FATAL $PICOARCH missing/not executable" >> "$LOG"
        sync
        sleep 5
        continue
    fi
    "$PICOARCH" "$FROGUI_CORE" "$FROGUI_CORE" >> "$LOG" 2>&1
    RC=$?
    echo "frogui exited rc=$RC" >> "$LOG"
    sync
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
            sync
        fi
    fi
    sleep 0.2
done
