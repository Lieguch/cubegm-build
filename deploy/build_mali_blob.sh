#!/usr/bin/env bash
# =============================================================================
#  build_mali_blob.sh -- Install libmali-rk-utgard-400 fbdev blob + headers into sysroot
# -----------------------------------------------------------------------------
#  RK3036G has Mali-400 MP GPU. Kernel driver is already loaded (diag_probe
#  confirmed: /dev/mali exists, debugfs /sys/kernel/debug/mali has full
#  Mali-400 MP entries, /dev/dri/renderD128 exists).
#
#  This script installs the ARM binary blob (libmali-utgard-400-r7p0-r0p0-fbdev.so)
#  and EGL/GLES2 headers from paolosabatino/libmali-rk-utgard-400 (which packages
#  ARM's official Rockchip Mali user-space drivers).
#
#  The blob provides: libEGL.so, libGLESv2.so, libGLESv1_CM.so, libmali.so
#  RetroArch links against these via --enable-mali_fbdev --enable-opengles --enable-egl
#
#  Idempotent: re-running skips already-installed components.
# =============================================================================
set -euo pipefail

SYSROOT="${SYSROOT:?SYSROOT must be set}"
MALI_REPO="https://github.com/paolosabatino/libmali-rk-utgard-400"
BLOB_NAME="libmali-utgard-400-r7p0-r0p0-fbdev.so"
BLOB_URL="${MALI_REPO}/raw/master/lib/arm-linux-gnueabihf/${BLOB_NAME}"

log(){ printf '\033[1;34m[mali]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

[ -d "$SYSROOT/usr" ] || die "SYSROOT $SYSROOT/usr missing"
mkdir -p "$SYSROOT/usr/lib" "$SYSROOT/usr/include/EGL" "$SYSROOT/usr/include/GLES2" "$SYSROOT/usr/include/GLES"

# --- 1. Download blob if not cached -----------------------------------------
WORK="$(pwd)/.mali_build"
mkdir -p "$WORK"
BLOB_DST="$SYSROOT/usr/lib/$BLOB_NAME"

if [ -f "$BLOB_DST" ]; then
    log "libmali blob already in sysroot -- skip"
else
    log "Downloading libmali fbdev blob..."
    # GitHub API base64 download (works through GFW where raw.githubusercontent fails)
    if curl -s -m 120 "https://api.github.com/repos/paolosabatino/libmali-rk-utgard-400/contents/lib/arm-linux-gnueabihf/${BLOB_NAME}" \
        | python3 -c "import sys,json,base64; d=json.load(sys.stdin); open('${BLOB_DST}','wb').write(base64.b64decode(d['content']))" 2>/dev/null; then
        log "blob downloaded via GitHub API"
    elif curl -sL -m 120 -o "$BLOB_DST" "$BLOB_URL"; then
        log "blob downloaded via raw"
    else
        die "Failed to download libmali blob"
    fi
    [ -s "$BLOB_DST" ] || die "blob file is empty after download"
fi

# --- 2. Create symlinks (libEGL.so, libGLESv2.so, etc.) ---------------------
log "Creating symlinks..."
cd "$SYSROOT/usr/lib"
for link in libmali.so libmali.so.7 libmali.so.7.0.0; do
    ln -sf "$BLOB_NAME" "$link"
done
ln -sf "$BLOB_NAME" libEGL.so
ln -sf "$BLOB_NAME" libEGL.so.1
ln -sf "$BLOB_NAME" libGLESv2.so
ln -sf "$BLOB_NAME" libGLESv2.so.2
ln -sf "$BLOB_NAME" libGLESv1_CM.so
ln -sf "$BLOB_NAME" libGLESv1_CM.so.1
cd - >/dev/null

# --- 3. Download headers if not present ------------------------------------
# 双源下载：jsdelivr 快速通道 → 失败(空文件/非200)自动回退 GitHub API base64
# (504 教训：jsdelivr 静默失败导致 sysroot 无 EGL header，configure die)
log "Installing headers..."
HDR_BASE="https://cdn.jsdelivr.net/gh/paolosabatino/libmali-rk-utgard-400@master/include"

fetch_hdr() {
    # $1 = repo-relative path (e.g. EGL/egl.h)
    local src="$1"
    local dst="$SYSROOT/usr/include/$src"
    [ -s "$dst" ] && return 0
    local dir=$(dirname "$dst")
    mkdir -p "$dir"
    # 通道1: jsdelivr
    curl -s -m 40 -o "$dst" "${HDR_BASE}/${src}" || true
    [ -s "$dst" ] && { log "header $src via jsdelivr ($(wc -c < "$dst") bytes)"; return 0; }
    rm -f "$dst"
    # 通道2: GitHub API base64（同 blob 下载通道，CI 已验证可达）
    curl -s -m 60 "https://api.github.com/repos/paolosabatino/libmali-rk-utgard-400/contents/$src" \
        | python3 -c "import sys,json,base64; d=json.load(sys.stdin); open('${dst}','wb').write(base64.b64decode(d['content']))" \
        || { log "WARN: both channels failed for $src"; return 1; }
    [ -s "$dst" ] && log "header $src via GitHub API ($(wc -c < "$dst") bytes)"
}

for src in \
    "EGL/egl.h" "EGL/eglext.h" "EGL/eglplatform.h" \
    "GLES2/gl2.h" "GLES2/gl2ext.h" \
    "GLES/gl.h" \
    "gbm.h" "mali.icd"
do
    fetch_hdr "$src" || true
done

# --- 4. 安装校验：关键 header 必须落盘，否则 RA configure 必 die ---
for must in EGL/egl.h EGL/eglext.h EGL/eglplatform.h GLES2/gl2.h GLES2/gl2ext.h; do
    [ -s "$SYSROOT/usr/include/$must" ] || die "FATAL: $must missing in sysroot -- header download failed on all channels"
done
log "=== sysroot mali include tree ==="
find "$SYSROOT/usr/include/EGL" "$SYSROOT/usr/include/GLES2" "$SYSROOT/usr/include/GLES" -type f 2>/dev/null

log "=== libmali staged into $SYSROOT/usr/lib ==="
ls -la "$SYSROOT/usr/lib/libmali"* "$SYSROOT/usr/lib/libEGL"* "$SYSROOT/usr/lib/libGLES"* 2>/dev/null
log "DONE. RetroArch can now link -lEGL -lGLESv2 -lmali against this sysroot."
