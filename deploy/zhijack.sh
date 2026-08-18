#!/bin/sh
# CubeGM autorun hijack — open-source frontend launcher (picoarch + FrogUI)
#
# Invoked by the stock launcher via setting.xml:
#   <autorun file="cubegm/zhijack.sh" driver="" />
#
# SAFETY (per architecture.md §四 / cubegm_replacement_feasibility.md §五.5):
#   This script does NOT replace or modify any stock binary (rkgame/icube/
#   driver.so). It only launches our own binaries that live alongside them,
#   so the device boot checksum is never tripped ("sdcard is damaged").
#
# Boot chain (verified from analysis docs):
#   stock rkgame -> autorun -> cubegm/zhijack.sh
#       -> picoarch ./cores/frogui_libretro.so   (FrogUI menu core)
#           -> user picks ROM -> fork() runs the selected libretro core

set -e

# Resolve our own directory so relative paths (cores/, Roms/) resolve on the SD card.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR" || exit 1

# FrogUI is itself a libretro core; picoarch loads it as the front-end menu.
# (LAUNCH_FILE=/tmp/frogui_launch.txt + RETRO_ENVIRONMENT_SHUTDOWN are handled
#  internally by picoarch/FrogUI — no env setup needed here.)
if [ ! -x ./picoarch ]; then
    echo "cubegm/zhijack.sh: ./picoarch not found in $SCRIPT_DIR" >&2
    exit 1
fi
if [ ! -e ./cores/frogui_libretro.so ]; then
    echo "cubegm/zhijack.sh: ./cores/frogui_libretro.so not found" >&2
    exit 1
fi

exec ./picoarch ./cores/frogui_libretro.so
