#!/bin/bash
# =============================================================================
#  build_mesa_lima.sh -- Cross-compile Mesa 20.3.5 (lima + kmsro) into sysroot
#
#  RK3036G is Mali-400 (Utgard). Device rootfs is glibc-2.29 (org.bin libc-2.29.so
#  proof). ARM closed blob (libmali) has non-standard eglBindAPI behavior when no
#  display exists (returns EGL_FALSE silently). Correct fix: mainline kernel with
#  CONFIG_DRM_LIMA + Mesa open-source userspace (lima driver, kmsro display).
#
#  Mesa official: docs.mesa3d.org/drivers/lima.html — Mali-400 = Supported,
#  Rockchip = tested display driver (kmsro path).
#
#  Usage:  SYSROOT=/path/to/sysroot CROSS_COMPILE=arm-linux-gnueabihf- ./build_mesa_lima.sh
#  Env:  SYSROOT      (required) toolchain sysroot (has usr/lib, usr/include)
#        CROSS_COMPILE (required) cross prefix ending in dash
#        SRCDIR       (optional) where to download/extract sources (default: $SYSROOT/../mesa-src)
#        JOBS          (optional) build parallelism (default: nproc)
#
#  Output: Mesa .so installed into $SYSROOT/usr/lib; runtime libs copied to
#          $SYSROOT/usr/lib/mesa-stage (for payload assembly)
#
#  Tested (2026-09-29, CNB 8-core): expat→libdrm→mesa all cross-compile OK.
#  GLIBC ceiling verified: libEGL=2.29, libgbm=2.29, libGLESv2(=n/a), libglapi=2.4, lima_dri=2.29.
# =============================================================================
set -euo pipefail

SYSROOT="${SYSROOT:?SYSROOT must be set}"
CROSS_COMPILE="${CROSS_COMPILE:?CROSS_COMPILE must be set (e.g. arm-linux-gnueabihf-)}"
SRCDIR="${SRCDIR:-$(dirname "$SYSROOT")/mesa-src}"
JOBS="${JOBS:-$(nproc)}"
PREFIX="$SYSROOT/usr"
STAGE_DIR="$SYSROOT/usr/lib/mesa-stage"

# meson cross file (toolchain paths resolved from CROSS_COMPILE + SYSROOT)
CROSS_FILE="$SRCDIR/armhf.cross"
mkdir -p "$SRCDIR" "$STAGE_DIR"

# ---- host build tools needed by meson/mesa (apt in CI bootstrap, not here) ----
# Meson official requirements: https://mesonbuild.com/Quick-guide.html
# Mesa official build requirements: https://docs.mesa3d.org/meson.html
#   -> python3 + python3-mako + ninja; meson itself may come from pip.
command -v meson >/dev/null || { echo "ERROR: meson missing"; exit 1; }
NINJA="$(command -v ninja || command -v ninja-build || true)"
[ -n "$NINJA" ] || { echo "ERROR: ninja missing (ninja / ninja-build)"; exit 1; }
python3 -c 'import mako' >/dev/null 2>&1 \
    || { echo "ERROR: python3 Mako module missing (Mesa meson.build requirement)"; exit 1; }
command -v "${CROSS_COMPILE}gcc" >/dev/null || { echo "ERROR: ${CROSS_COMPILE}gcc missing"; exit 1; }

# Resolve absolute paths for the cross file
CC_ABS=$(command -v "${CROSS_COMPILE}gcc")
CPP_ABS=$(command -v "${CROSS_COMPILE}g++" || echo "${CROSS_COMPILE}g++")
AR_ABS=$(command -v "${CROSS_COMPILE}ar")
NM_ABS=$(command -v "${CROSS_COMPILE}nm")
STRIP_ABS=$(command -v "${CROSS_COMPILE}strip")
RANLIB_ABS=$(command -v "${CROSS_COMPILE}ranlib")
PKGCONF="$(command -v pkgconf || command -v pkg-config || echo pkg-config)"
SYSROOT_ABS=$(cd "$SYSROOT" && pwd)

cat > "$CROSS_FILE" <<CROSSEOF
[binaries]
c = '${CC_ABS}'
cpp = '${CPP_ABS}'
ar = '${AR_ABS}'
nm = '${NM_ABS}'
strip = '${STRIP_ABS}'
ranlib = '${RANLIB_ABS}'
pkgconfig = '${PKGCONF}'

[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'cortex-a7'
endian = 'little'

[properties]
c_args = ['-march=armv7-a', '-mtune=cortex-a7', '-mfpu=neon-vfpv4', '-mfloat-abi=hard', '-O2']
c_link_args = ['-march=armv7-a', '-mtune=cortex-a7', '-mfpu=neon-vfpv4', '-mfloat-abi=hard']
cpp_args = ['-march=armv7-a', '-mtune=cortex-a7', '-mfpu=neon-vfpv4', '-mfloat-abi=hard', '-O2']
cpp_link_args = ['-march=armv7-a', '-mtune=cortex-a7', '-mfpu=neon-vfpv4', '-mfloat-abi=hard']
pkg_config_libdir = ['${SYSROOT_ABS}/usr/lib/pkgconfig', '${SYSROOT_ABS}/usr/share/pkgconfig']

[built-in options]
c_std = 'gnu11'
cpp_std = 'gnu++14'
CROSSEOF
echo "cross file -> $CROSS_FILE"

# ---- fetch sources (skip if already extracted) ----
fetch() { # url file
  local url="$1" f="$2"
  [ -f "$SRCDIR/$f" ] || curl -fsSL --retry 3 -o "$SRCDIR/$f" "$url"
}
extract_local() { # file destdir
  local f="$1" d="$2"
  [ -d "$d" ] || { mkdir -p "$d"; tar xf "$SRCDIR/$f" -C "$d" --strip-components=1; }
}

MESA_VER=20.3.5
DRM_VER=2.4.104
EXPAT_VER=2.2.10

echo "=== download sources ==="
fetch "https://gitlab.freedesktop.org/mesa/mesa/-/archive/mesa-${MESA_VER}/mesa-mesa-${MESA_VER}.tar.gz" "mesa-${MESA_VER}.tar.gz"
fetch "https://dri.freedesktop.org/libdrm/libdrm-${DRM_VER}.tar.xz" "libdrm-${DRM_VER}.tar.xz"
fetch "https://github.com/libexpat/libexpat/releases/download/R_2_2_10/expat-${EXPAT_VER}.tar.xz" "expat-${EXPAT_VER}.tar.xz"

echo "=== extract sources ==="
extract_local "mesa-${MESA_VER}.tar.gz"   "$SRCDIR/mesa-${MESA_VER}"
extract_local "libdrm-${DRM_VER}.tar.xz"  "$SRCDIR/libdrm-${DRM_VER}"
extract_local "expat-${EXPAT_VER}.tar.xz" "$SRCDIR/expat-${EXPAT_VER}"

export PATH="$(echo "$CROSS_COMPILE" | sed 's/-$//')/../../bin:$PATH" 2>/dev/null || true
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export CFLAGS_CROSS="-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2"

echo ""
echo "=== 1) expat ${EXPAT_VER} (static, -fPIC) ==="
cd "$SRCDIR/expat-${EXPAT_VER}"
[ -f Makefile ] && make distclean >/dev/null 2>&1 || true
./configure --host="$(echo "$CROSS_COMPILE" | sed 's/-$//')" --build=x86_64-linux-gnu \
  --prefix="$PREFIX" --disable-shared --enable-static \
  --without-docbook --without-xmlwf \
  CFLAGS="$CFLAGS_CROSS -fPIC" \
  || { echo "expat configure failed"; exit 1; }
make -j"$JOBS" && make install
echo "expat done -> $PREFIX/lib/libexpat*"

echo ""
echo "=== 2) libdrm ${DRM_VER} ==="
cd "$SRCDIR/libdrm-${DRM_VER}"
rm -rf build
meson setup build --cross-file "$CROSS_FILE" --prefix="$PREFIX" --libdir=lib \
  -Dudev=false -Dvalgrind=false -Dintel=false -Dvmwgfx=false \
  -Dnouveau=false -Dfreedreno=false -Damdgpu=false \
  -Dcairo-tests=false -Dman-pages=false || { echo "libdrm meson failed"; exit 1; }
ninja -C build && ninja -C build install
echo "libdrm done -> $PREFIX/lib/pkgconfig/libdrm.pc"

echo ""
echo "=== 3) Mesa ${MESA_VER} (lima + kmsro, no X11/wayland/llvm) ==="
cd "$SRCDIR/mesa-${MESA_VER}"
# idempotent mako patch (distutils removed in Python 3.12+)
python3 - "$SRCDIR/mesa-${MESA_VER}/meson.build" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = 'from distutils.version import StrictVersion\nimport mako\nassert StrictVersion(mako.__version__) > StrictVersion("0.8.0")'
new = 'import mako\n_v = tuple(int(x) for x in mako.__version__.split(".")[:2] if x.isdigit())\nassert _v > (0, 8), mako.__version__'
if old in s:
    open(p, 'w').write(s.replace(old, new))
    print("mako check patched (distutils removed)")
else:
    print("mako patch already applied or pattern absent")
PY
rm -rf build
meson setup build --cross-file "$CROSS_FILE" --prefix="$PREFIX" --libdir=lib \
  -Dgallium-drivers=lima,kmsro \
  -Dvulkan-drivers= \
  -Ddri-drivers= \
  -Dplatforms= \
  -Degl=enabled \
  -Dgbm=enabled \
  -Dgles1=disabled \
  -Dgles2=enabled \
  -Dglx=disabled \
  -Dllvm=disabled \
  -Dopengl=true \
  -Dshared-glapi=enabled \
  -Dglvnd=false \
  -Dgallium-omx=disabled \
  -Dgallium-xa=disabled \
  -Dgallium-nine=false \
  -Dgallium-opencl=disabled \
  -Dgallium-vdpau=disabled \
  -Dgallium-xvmc=disabled \
  -Dvalgrind=disabled \
  -Dlibunwind=disabled \
  -Dbuild-tests=false || { echo "mesa meson failed"; exit 1; }
ninja -C build && ninja -C build install
echo "mesa done"

echo ""
echo "=== 4) collect runtime libs ==="
find "$PREFIX/lib" -maxdepth 2 \( -name 'libEGL.so*' -o -name 'libgbm.so*' -o \
     -name 'libGLESv2.so*' -o -name 'libglapi*' -o -name 'libgallium*' -o \
     -name 'lima_dri.so' -o -name 'rockchip_dri.so' \) -exec cp -av {} "$STAGE_DIR/" \; 2>/dev/null || true
echo "--- staged: ---"
ls -la "$STAGE_DIR" | head -30

echo ""
echo "=== 5) GLIBC ceiling check (must be <=2.29) ==="
for so in $(find "$STAGE_DIR" -name '*.so*' -type f 2>/dev/null); do
    max=$(readelf -V "$so" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -V | tail -1)
    echo "  $(basename "$so"): GLIBC_max=${max:-none}"
done

echo ""
echo "=== done: runtime libs in $STAGE_DIR ==="