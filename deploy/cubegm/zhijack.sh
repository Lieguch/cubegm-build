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
# v10.2: rotate stale per-boot logs so each boot starts clean (previous boot's
# copy kept as .old for post-mortem; prevents unbounded growth on the FAT card).
for _l in retroarch.log picoarch_init.log tfhijack.log icube.log; do
    [ -f "/mnt/sdcard/$_l" ] && mv -f "/mnt/sdcard/$_l" "/mnt/sdcard/$_l.old" 2>/dev/null
done

echo "=== zhijack boot [rk3036g] $(date '+%H:%M:%S' 2>/dev/null) ===" >> "$LOG"
echo "pid=$$ cmdline=$(cat /proc/$$/cmdline 2>/dev/null)" >> "$LOG"
# v8.7: prove the debug/crash-log path is live. picoarch appends a backtrace
# here on SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE; this header line exists so
# "no crash.log" can be distinguished from "no crash happened" next round.
echo "=== CubeGM debug log (crash backtraces land here) $(date '+%H:%M:%S' 2>/dev/null) ===" > /mnt/sdcard/crash.log
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

# SDL 1.2 fbcon mouse: RK3036G has no PS/2 mouse device (/dev/input/mice,
# /dev/usbmouse, /dev/psaux). FB_OpenMouse() fails, SDL_InitVideo returns -1
# ("Unable to open mouse") — the root cause of the black screen + FrogUI
# restart loop. SDL_NOMOUSE is SDL's own documented switch (SDL_fbvideo.c:802)
# that skips the mouse probe error. The device is gamepad-only, no PS/2 mouse.
export SDL_NOMOUSE=1

# Runtime libs the device rootfs does NOT provide (SDL/libpng12/z) ship in
# cubegm/lib; picoarch's NEEDED entries resolve against it.
export LD_LIBRARY_PATH=/mnt/sdcard/cubegm/lib:/mnt/sdcard/cubegm/usr/lib:$LD_LIBRARY_PATH

# v11.7 音频根治（真根因）：payload 的 libasound.so.2 由 crosstool sysroot 编译，
# ALSA_CONFIG_DIR 写死为 CI 机路径（设备不存在）-> 官方 alsa.conf 加载不到 ->
# Unknown PCM default。用官方 ALSA_CONFIG_PATH 直指自包含 asound.conf。
# HOME/XDG_CONFIG_HOME 保留：保证两条启动路径一致 + 配置路径正确。
export HOME=/mnt/sdcard/cubegm
export XDG_CONFIG_HOME=/mnt/sdcard/cubegm/configs
export ALSA_CONFIG_PATH=/mnt/sdcard/cubegm/asound.conf

# CPU: force max-performance governor (helps every emulator).
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -w "$g" ] && echo performance > "$g" 2>/dev/null
done
for c in /sys/devices/system/cpu/cpu*/cpufreq; do
    mx=$(cat "$c/cpuinfo_max_freq" 2>/dev/null)
    [ -n "$mx" ] && [ -w "$c/scaling_min_freq" ] && echo "$mx" > "$c/scaling_min_freq" 2>/dev/null
done
# cpufreq state (diagnostic: games at 1-4 fps could be a throttled CPU)
for c in /sys/devices/system/cpu/cpu*/cpufreq; do
    [ -e "$c/scaling_governor" ] || continue
    echo "zhijack: cpufreq gov=$(cat "$c/scaling_governor" 2>/dev/null) cur=$(cat "$c/scaling_cur_freq" 2>/dev/null) min=$(cat "$c/scaling_min_freq" 2>/dev/null) max=$(cat "$c/cpuinfo_max_freq" 2>/dev/null)" >> "$LOG"
done
cat /proc/meminfo 2>/dev/null | head -6 >> "$LOG"

# --- fb0 link test: full-white frame (RGB565 0xFFFF / 32-bit 0xFFFFFFFF) -------
echo "zhijack: fb0 white-frame test..." >> "$LOG"
if [ -e /dev/fb0 ]; then
    head -c 1843200 /dev/zero 2>/dev/null | tr '\000' '\377' > /dev/fb0 2>>"$LOG"
    echo "zhijack: fb0 white frame rc=$? (white flash visible = fb0->HDMI OK)" >> "$LOG"
else
    echo "zhijack: /dev/fb0 NOT PRESENT -> fb0 path cannot work" >> "$LOG"
fi

# --- SD card layout (ROM root diagnostics: stock card ships Roms/; FrogUI
# probes roms then Roms. "11.png" visible on the card but black menu = path
# mismatch) ----------------------------------------------------------------
echo "=== SD card layout ===" >> "$LOG"
ls /mnt/sdcard/ 2>/dev/null | head -30 >> "$LOG"
if [ -d /mnt/sdcard/roms ]; then  echo "roms DIR EXISTS" >> "$LOG";  ls /mnt/sdcard/roms/ 2>/dev/null | head -30 >> "$LOG";  else echo "roms DIR MISSING" >> "$LOG"; fi
if [ -d /mnt/sdcard/Roms ]; then  echo "Roms DIR EXISTS" >> "$LOG";  ls /mnt/sdcard/Roms/ 2>/dev/null | head -30 >> "$LOG";  else echo "Roms DIR MISSING" >> "$LOG"; fi
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

# RK3036G input is standard evdev. RetroArch (primary frontend, build.sh STAGE 5b)
# is self-contained: SDL1.2 fbcon video + ALSA audio + linuxraw input (reads
# /dev/input/event* directly) + RGUI menu, all built in. No cubevol/gpio shim
# required (driver.so has no cubevol references). picoarch+FrogUI deprecated.

RETROARCH=/mnt/sdcard/cubegm/retroarch
RETROARCH_CFG=/mnt/sdcard/cubegm/retroarch.cfg

if [ -e /dev/watchdog ]; then
    echo "zhijack: /dev/watchdog PRESENT -- petting each loop" >> "$LOG"
fi
echo "zhijack: past bridge start, entering backup restoration" >> "$LOG"

# NEVER rename/delete the stock icube/rkgame binaries. A previous build renamed
# icube -> icube.bak to "disarm" the respawner; on the NEXT boot the stock init
# could not find icube and the device hung on the boot logo (half-white + stock
# logo, unrecoverable by reboot). Restore any .bak first, then FREEZE (SIGSTOP)
# the respawner -- the binary stays intact so the stock chain can always boot.
for _name in icube rkgame; do
    _bak="/mnt/sdcard/cubegm/$_name.bak"
    if [ -f "$_bak" ]; then
        mv "$_bak" "/mnt/sdcard/cubegm/$_name" 2>>"$LOG" && echo "zhijack: RESTORED $_bak -> $_name (boot chain repaired)" >> "$LOG"
    fi
done
echo "zhijack: past backup restoration, freezing respawner" >> "$LOG"
kill -STOP $(pidof icube) 2>/dev/null
killall -9 rkgame 2>/dev/null
echo "zhijack: respawner frozen (icube pidof=$(pidof icube 2>/dev/null))" >> "$LOG"

# Background icube/rkgame watchdog: the main loop below only runs BETWEEN game
# launches -- while FrogUI/game is up nothing stops the respawner, so icube
# revives after ~10-15 min, respawns rkgame and steals DRM master back (the
# observed half-white + stock boot logo screen). SIGSTOP + kill -9 every 1 s
# from a setsid-detached loop (kill only, never renames anything). Every 30 s
# it logs what it sees, so the NEXT test tells us exactly who revives and
# whether the watchdog is still alive at the half-white moment.
setsid sh -c '
    WD_LOG="$1"
    i=0
    while true; do
        i=$((i+1))
        # STOP any existing instance first (freezes its respawn thread), then
        # kill -9 — STOP-then-kill closes the race where a fresh icube runs
        # between our kill and the next iteration.
        for p in $(pidof icube 2>/dev/null); do kill -STOP "$p" 2>/dev/null; done
        for p in $(pidof rkgame 2>/dev/null); do kill -STOP "$p" 2>/dev/null; done
        killall -9 icube rkgame 2>/dev/null
        if [ $((i % 30)) -eq 0 ]; then
            echo "watchdog alive@${i}s icube=$(pidof icube 2>/dev/null) rkgame=$(pidof rkgame 2>/dev/null) up=$(cat /proc/uptime 2>/dev/null | cut -d. -f1)s" >> "$WD_LOG"
        fi
        sleep 1
    done
' sh "$LOG" &
WD_PID=$!
echo "zhijack: past watchdog start (WD_PID=$WD_PID)" >> "$LOG"

# Front-end launcher loop.
# ONLY frontend: RetroArch — self-contained (menu/ROM browser/cores),
# crash-restart via the loop below. picoarch+FrogUI fully removed (v10.0).
echo "zhijack: entering main loop (retroarch=$([ -x "$RETROARCH" ] && echo yes || echo no))" >> "$LOG"
ITER=0
while true; do
    ITER=$((ITER+1))
    # icube is the respawner: if it revives (crash-restart), it respawns
    # rkgame which takes DRM master back -> HDMI switches away from our dumb
    # buffer (observed as half-white after ~10 min). Kill -9 it every iteration.
    killall -9 icube 2>/dev/null
    killall -9 rkgame 2>/dev/null
    # pet the stock watchdog if present (prevents a ~10-min system reset)
    [ -e /dev/watchdog ] && echo > /dev/watchdog 2>/dev/null
    echo "--- iter $ITER: frontend (icube=$(pidof icube 2>/dev/null)) ---" >> "$LOG"
    # v8.7: stamp each frontend launch into crash.log — proves the debug path
    # runs every boot, so a future "no crash.log" is a real no-crash, not a
    # missing feature.
    echo "=== frontend launch iter=$ITER $(date '+%H:%M:%S' 2>/dev/null) ===" >> /mnt/sdcard/crash.log 2>/dev/null
    # Direction B: diagnostics run on EVERY boot (no flag file needed).
    # Outputs structured report to /mnt/sdcard/diag_report.txt covering
    # ALSA cards, PCM devices, acodec/I2S/HDMI registers, DRM, input,
    # libretro core scan, and 1 kHz audio test. Read report from SD card.
    if [ -x /mnt/sdcard/cubegm/diag ]; then
      echo "zhijack: running diag all..." >> "$LOG"
      /mnt/sdcard/cubegm/diag all >> "$LOG" 2>&1
      echo "zhijack: diag finished, report -> /mnt/sdcard/diag_report.txt" >> "$LOG"
      # v10.9 开机即 Debug：持续键位/轴事件日志（守护进程，不阻塞）
      /mnt/sdcard/cubegm/diag keylog >/dev/null 2>&1 &
      echo "zhijack: diag keylog started (bg) -> /mnt/sdcard/keylog.txt" >> "$LOG"
    fi
    if [ -x "$RETROARCH" ]; then
        # PRIMARY PATH: RetroArch owns the whole session (RGUI menu -> game ->
        # back to menu). Video = SDL 1.2 fbcon, audio = ALSA, input = linuxraw
        # (reads /dev/input/event* directly). --menu keeps RGUI resident.
        echo "=== retroarch launch iter=$ITER $(date '+%H:%M:%S' 2>/dev/null) ===" >> /mnt/sdcard/crash.log 2>/dev/null
        "$RETROARCH" -c "$RETROARCH_CFG" --menu >> "$LOG" 2>&1
        RC=$?
        echo "retroarch exited rc=$RC (cooldown 1s)" >> "$LOG"
        sleep 1
        continue
    fi
    # RetroArch is the only frontend; picoarch+FrogUI deprecated (v10.0)
    # If RetroArch binary missing, fatal error
    if [ ! -x "$RETROARCH" ]; then
        echo "zhijack: FATAL $RETROARCH not executable -- cannot launch" >> "$LOG"
        sleep 5
        continue
    fi
done
