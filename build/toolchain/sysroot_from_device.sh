#!/usr/bin/env bash
# sysroot_from_device.sh -- dump the RK3036G device rootfs to use as the
# cross-compile sysroot. This guarantees glibc/ABI match (device = glibc 2.17).
#
# Run on a machine that can reach the handheld over ssh. The device must be
# booted into a shell (stock firmware often exposes telnet/adb, or you can drop
# to a root shell). Adjust DEVICE below.
set -e
DEVICE="${DEVICE:-root@cubegm.local}"     # e.g. root@192.168.1.50
SYSROOT="${SYSROOT:-$HOME/cubegm-sysroot}"

echo "Pulling rootfs from $DEVICE -> $SYSROOT"
mkdir -p "$SYSROOT"

# Pull the libraries + headers we need to link against.
ssh "$DEVICE" 'cd / && tar czf - lib usr/lib usr/include opt 2>/dev/null' \
  | tar xzf - -C "$SYSROOT"

# Report the glibc version we captured (sanity check).
LIBC=$(find "$SYSROOT" -name 'libc.so.6' 2>/dev/null | head -1)
if [ -n "$LIBC" ]; then
  echo "Captured libc: $LIBC"
  "$SYSROOT/../.." >/dev/null 2>&1 || true
fi
echo
echo "SYSROOT ready: $SYSROOT"
echo "Use it when cross-compiling, e.g.:"
echo "  make SYSROOT=$SYSROOT"
