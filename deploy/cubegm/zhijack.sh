#!/bin/sh
# zhijack.sh -- invoked by the stock firmware's 'autorun'.
# Boots our picoarch + FrogUI front-end in place of the stock menu.
#
# SAFETY: this script must NOT modify root.dat or any checksum/partition data,
# otherwise the device shows "sdcard is damaged" and refuses to boot. We only
# add our launch path; the stock boot chain (rkgame/icube/driver.so) is left
# fully intact upstream of this point.
set -e

CUBEGM_DIR="$(dirname "$0")"
export LD_LIBRARY_PATH="$CUBEGM_DIR/lib:$LD_LIBRARY_PATH"
export PATH="$CUBEGM_DIR/bin:$PATH"

# picoarch = libretro front-end; FrogUI (frogui_libretro.so) = the launcher/
# desktop core it loads first. From there the user picks a game and picoarch
# dlopen()s the selected standard libretro core into the same process.
if [ -x "$CUBEGM_DIR/picoarch" ] && [ -f "$CUBEGM_DIR/frogui_libretro.so" ]; then
    exec "$CUBEGM_DIR/picoarch" "$CUBEGM_DIR/frogui_libretro.so"
else
    echo "CubeGM replacement binaries missing (picoarch/frogui_libretro.so)." >&2
    echo "Run deploy/build.sh on your Linux PC first, then copy cubegm/ to SD." >&2
    exit 1
fi
