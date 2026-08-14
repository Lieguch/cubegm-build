#!/bin/sh
# zhijack.sh -- invoked by the stock firmware's 'autorun'.
# Boots our picoarch + FrogUI front-end in place of the stock menu.
#
# SAFETY: this script must NOT modify root.dat or any checksum/partition data,
# otherwise the device shows "sdcard is damaged" and refuses to boot. We only
# add our launch path; the stock boot chain is left intact upstream.
set -e

CUBEGM_DIR="$(dirname "$0")"
export LD_LIBRARY_PATH="$CUBEGM_DIR/lib:$LD_LIBRARY_PATH"
export PATH="$CUBEGM_DIR/bin:$PATH"

# picoarch is the libretro front-end; FrogUI (frogui_libretro.so) is the
# launcher/desktop core it loads first. From there the user picks a game and
# picoarch fork()s the selected libemu_*.so core.
exec "$CUBEGM_DIR/picoarch" "$CUBEGM_DIR/frogui_libretro.so"
