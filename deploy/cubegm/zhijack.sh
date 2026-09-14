#!/bin/sh
# =============================================================================
#  zhijack.sh -- RK3036G (ARM, HDMI 1280x720, standard Linux DRM/ALSA/evdev)
# =============================================================================
#  Reached via the stock boot chain (fallback path; primary = icube_replacement):
#    rkgame (untouched) -> setting.xml autorun -> cubegm/zhijack.sh
#
#  release-1.0 (2026-09-14, 基于 478=c82e9c5): 去掉全部日志生成。
#    - 不再写 zhijack.log / crash.log / diag_report.txt / keylog.txt；
#    - 不再自动运行 diag（诊断二进制仍随 payload 部署，手动执行
#      /mnt/sdcard/cubegm/diag <module> 可用）；
#    - retroarch 子进程输出静默（/dev/null）。
#    功能逻辑与 478 完全一致：icube/rkgame 冻结、tfdevice.env、SDL_NOMOUSE、
#    LD_LIBRARY_PATH、HOME/XDG_CONFIG_HOME、CPU performance、watchdog、
#    .bak 恢复、主循环 retroarch 崩溃重启。
#
#  SAFETY: this script never touches root.dat or any checksum/partition data.
# =============================================================================
mkdir /tmp/zhijack.lock 2>/dev/null || exit 0

# Freeze icube (the respawner) THEN kill rkgame so the stock menu can't redraw
# over our frames and nothing respawns a fresh rkgame.
kill -STOP $(pidof icube) 2>/dev/null
killall rkgame 2>/dev/null

# Device environment for frontends that read /tmp/tfdevice.env.
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
# /dev/usbmouse, /dev/psaux). FB_OpenMouse() fails and SDL_InitVideo returns -1
# ("Unable to open mouse") — the root cause of the black screen + FrogUI
# restart loop. SDL_NOMOUSE is SDL's own documented switch (SDL_fbvideo.c:802)
# that skips the mouse probe error. The device is gamepad-only, no PS/2 mouse.
export SDL_NOMOUSE=1

# Runtime libs the device rootfs does NOT provide (SDL/libpng12/z) ship in
# cubegm/lib; retroarch's NEEDED entries resolve against it.
export LD_LIBRARY_PATH=/mnt/sdcard/cubegm/lib:/mnt/sdcard/cubegm/usr/lib:$LD_LIBRARY_PATH

# v11.6 音频根治：不覆盖 ALSA_CONFIG_PATH（rootfs 自带官方 alsa.conf）。
# 官方 alsa.conf 的 @hooks 会按 HOME 自动加载 ~/.asoundrc（双输出定义，
# payload 已部署），使两条启动路径一致且 .asoundrc 生效。
export HOME=/mnt/sdcard/cubegm
export XDG_CONFIG_HOME=/mnt/sdcard/cubegm/configs

# CPU: force max-performance governor (helps every emulator).
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -w "$g" ] && echo performance > "$g" 2>/dev/null
done
for c in /sys/devices/system/cpu/cpu*/cpufreq; do
    mx=$(cat "$c/cpuinfo_max_freq" 2>/dev/null)
    [ -n "$mx" ] && [ -w "$c/scaling_min_freq" ] && echo "$mx" > "$c/scaling_min_freq" 2>/dev/null
done

RETROARCH=/mnt/sdcard/cubegm/retroarch
RETROARCH_CFG=/mnt/sdcard/cubegm/retroarch.cfg

# NEVER rename/delete the stock icube/rkgame binaries. A previous build renamed
# icube -> icube.bak to "disarm" the respawner; on the NEXT boot the stock init
# could not find icube and the device hung on the boot logo (half-white + stock
# logo, unrecoverable by reboot). Restore any .bak first, then FREEZE (SIGSTOP)
# the respawner -- the binary stays intact so the stock chain can always boot.
for _name in icube rkgame; do
    _bak="/mnt/sdcard/cubegm/$_name.bak"
    if [ -f "$_bak" ]; then
        mv "$_bak" "/mnt/sdcard/cubegm/$_name" 2>/dev/null
    fi
done
kill -STOP $(pidof icube) 2>/dev/null
killall -9 rkgame 2>/dev/null

# Background icube/rkgame watchdog: the main loop below only runs BETWEEN game
# launches -- while a game is up nothing stops the respawner, so icube revives
# after ~10-15 min, respawns rkgame and steals DRM master back (the observed
# half-white + stock boot logo screen). SIGSTOP + kill -9 every 1 s from a
# setsid-detached loop (kill only, never renames anything).
setsid sh -c '
    while true; do
        # STOP any existing instance first (freezes its respawn thread), then
        # kill -9 — STOP-then-kill closes the race where a fresh icube runs
        # between our kill and the next iteration.
        for p in $(pidof icube 2>/dev/null); do kill -STOP "$p" 2>/dev/null; done
        for p in $(pidof rkgame 2>/dev/null); do kill -STOP "$p" 2>/dev/null; done
        killall -9 icube rkgame 2>/dev/null
        sleep 1
    done
' &

# Front-end launcher loop (fallback path).
# ONLY frontend: RetroArch — self-contained (menu/ROM browser/cores),
# crash-restart via the loop below. picoarch+FrogUI fully removed (v10.0).
while true; do
    # icube is the respawner: if it revives (crash-restart), it respawns
    # rkgame which takes DRM master back -> HDMI switches away from our dumb
    # buffer (observed as half-white after ~10 min). Kill -9 it every iteration.
    killall -9 icube 2>/dev/null
    killall -9 rkgame 2>/dev/null
    # pet the stock watchdog if present (prevents a ~10-min system reset)
    [ -e /dev/watchdog ] && echo > /dev/watchdog 2>/dev/null
    if [ -x "$RETROARCH" ]; then
        # PRIMARY PATH: RetroArch owns the whole session (RGUI menu -> game ->
        # back to menu). Video = SDL 1.2 fbcon, audio = ALSA, input = linuxraw
        # (reads /dev/input/event* directly). --menu keeps RGUI resident.
        # release-1.0: 输出静默（不生成 retroarch.log / zhijack.log）。
        "$RETROARCH" -c "$RETROARCH_CFG" --menu >/dev/null 2>&1
        sleep 1
        continue
    fi
    # RetroArch is the only frontend; picoarch+FrogUI deprecated (v10.0)
    # If RetroArch binary missing, wait and retry (no log output in release-1.0)
    sleep 5
done
