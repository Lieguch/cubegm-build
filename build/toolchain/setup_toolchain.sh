#!/usr/bin/env bash
# setup_toolchain.sh -- fetch the ARM GNU 13.2 (Linux-hosted) cross compiler.
#
# IMPORTANT: this compiler's *bundled* sysroot is glibc 2.38. The RK3036G device
# only provides glibc 2.17 (measured from its 20 libemu_*.so cores). Therefore:
#   - the GCC binary itself can be new,
#   - but for LINKING you MUST pass --sysroot=<glibc-2.17 sysroot> so the binary
#     links against glibc 2.17, otherwise it will fail at runtime with
#     "version GLIBC_2.xx not found".
#   - The 2.17 sysroot can be the DEVICE ROOTFS (sysroot_from_device.sh, gold
#     standard) OR a SELF-BUILT one (build_sysroot_ctng.sh, crosstool-NG, no
#     device needed -- RECOMMENDED when rootfs is not obtainable).
#   - ALSA userspace headers (alsa/asound.h) are NOT in this toolchain's sysroot;
#     get them from the device rootfs, or add alsa-lib's include/ to -I, or from
#     a buildroot-built sysroot. (DRM drm/drm.h and evdev linux/input.h ARE present.)
#   - NOTE: static linking is RULED OUT -- picoarch dlopen's the core into its own
#     process (core.c:720), so it must share glibc 2.17 with libemu_*.so.
#
# Run on your Linux build host (the repo was originally built in a buildroot VM).
set -e
TOOLCHAIN_DIR="${TOOLCHAIN_DIR:-$HOME/cubegm-toolchain}"
URL="https://developer.arm.com/-/media/Files/downloads/gnu/13.2.rel1/binrel/arm-gnu-toolchain-13.2.rel1-x86_64-arm-none-linux-gnueabihf.tar.xz"
# SHA256 (verified after download to cubegm-work/toolchain/ on 2026-08-14).
SHA256="df0f4927a67d1fd366ff81e40bd8c385a9324fbdde60437a512d106215f257b3"

mkdir -p "$TOOLCHAIN_DIR"
ARCHIVE="$TOOLCHAIN_DIR/$(basename "$URL")"

if [ ! -f "$ARCHIVE" ]; then
  echo "downloading $URL"
  curl -fL -o "$ARCHIVE" "$URL"
fi

if [ -n "$SHA256" ]; then
  echo "verifying sha256"
  echo "$SHA256  $ARCHIVE" | sha256sum -c -
fi

echo "extracting"
tar -xf "$ARCHIVE" -C "$TOOLCHAIN_DIR"

GCC=$(find "$TOOLCHAIN_DIR" -name 'arm-none-linux-gnueabihf-gcc' -type f | head -1)
if [ -z "$GCC" ]; then echo "ERROR: gcc not found after extract"; exit 1; fi
BIN_DIR=$(dirname "$GCC")

echo
echo "# Add these to your shell / Makefile:"
echo "export PATH=\"$BIN_DIR:\$PATH\""
echo "export CC=arm-none-linux-gnueabihf-gcc"
echo "export CXX=arm-none-linux-gnueabihf-g++"
echo
echo "DONE. For LINKING, point --sysroot at a glibc-2.17 sysroot:"
echo "(device rootfs via sysroot_from_device.sh, or self-built via build_sysroot_ctng.sh). gcc can be new."
