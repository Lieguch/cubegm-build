#!/usr/bin/env bash
# =============================================================================
#  build_mali_blob.sh -- Install libmali-rk-utgard-400 fbdev blob + headers into sysroot
#  (v2: FBDEV headers + KHR + gl2platform + libMali.so SONAME)
# =============================================================================
set -euo pipefail

SYSROOT="${SYSROOT:?SYSROOT must be set}"
MALI_REPO="https://github.com/paolosabatino/libmali-rk-utgard-400"
BLOB_NAME="libmali-utgard-400-r7p0-r0p0-fbdev.so"
BLOB_URL="${MALI_REPO}/raw/master/lib/arm-linux-gnueabihf/${BLOB_NAME}"
API_BASE="https://api.github.com/repos/paolosabatino/libmali-rk-utgard-400/contents"

log(){ printf '\033[1;34m[mali]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

[ -d "$SYSROOT/usr" ] || die "SYSROOT $SYSROOT/usr missing"
mkdir -p "$SYSROOT/usr/lib" \
    "$SYSROOT/usr/include/EGL" \
    "$SYSROOT/usr/include/GLES2" \
    "$SYSROOT/usr/include/GLES" \
    "$SYSROOT/usr/include/KHR"

# --- 1. Download blob (GitHub API base64, 已验证 CI 可达) ---
BLOB_DST="$SYSROOT/usr/lib/$BLOB_NAME"
if [ -f "$BLOB_DST" ] && [ -s "$BLOB_DST" ]; then
    log "libmali blob already in sysroot -- skip"
else
    log "Downloading libmali fbdev blob via GitHub API..."
    curl -s -m 120 "${API_BASE}/lib/arm-linux-gnueabihf/${BLOB_NAME}" \
        | python3 -c "import sys,json,base64; d=json.load(sys.stdin); open('${BLOB_DST}','wb').write(base64.b64decode(d['content']))" \
        || die "blob download via GitHub API failed"
    [ -s "$BLOB_DST" ] || die "blob file is empty after download"
    log "blob: $(wc -c < "$BLOB_DST") bytes"
fi

# --- 2. Symlinks (官方 SONAME=libMali.so, 大小写!) ---
log "Creating symlinks..."
cd "$SYSROOT/usr/lib"
# 官方 SONAME (readelf -d 确认)
ln -sf "$BLOB_NAME" libMali.so
# 链接别名 (RA 用 -lEGL -lGLESv2 -lmali)
ln -sf "$BLOB_NAME" libEGL.so
ln -sf "$BLOB_NAME" libEGL.so.1
ln -sf "$BLOB_NAME" libGLESv2.so
ln -sf "$BLOB_NAME" libGLESv2.so.2
ln -sf "$BLOB_NAME" libGLESv1_CM.so
ln -sf "$BLOB_NAME" libGLESv1_CM.so.1
ln -sf "$BLOB_NAME" libmali.so
ln -sf "$BLOB_NAME" libmali.so.7
ln -sf "$BLOB_NAME" libmali.so.7.0.0
cd - >/dev/null

# --- 3. Download headers (官方 FBDEV 套头 + KHR + GLES2 全量) ---
# 根因: 通用 EGL/eglplatform.h 在 Linux 默认走 X11 分支 → 无 X11 头必挂
# 官方解法: 用 FBDEV/ 目录的 eglplatform.h (typedef fbdev_window*) + KHR/khrplatform.h
log "Installing headers (FBDEV EGL + KHR + GLES2)..."
fetch_hdr() {
    local rel="$1" dest="$2"
    [ -s "$dest" ] && return 0
    mkdir -p "$(dirname "$dest")"
    curl -s -m 60 "${API_BASE}/${rel}" \
        | python3 -c "import sys,json,base64; d=json.load(sys.stdin); open('${dest}','wb').write(base64.b64decode(d['content']))" \
        || { log "WARN: failed: $rel"; return 1; }
    [ -s "$dest" ] && log "  $rel -> $(wc -c < "$dest") bytes"
}

# FBDEV EGL headers (libmali 官方 fbdev 变体, 避免 X11 依赖)
fetch_hdr "include/FBDEV/egl.h"             "$SYSROOT/usr/include/EGL/egl.h"
fetch_hdr "include/FBDEV/eglext.h"         "$SYSROOT/usr/include/EGL/eglext.h"
fetch_hdr "include/FBDEV/eglplatform.h"     "$SYSROOT/usr/include/EGL/eglplatform.h"
fetch_hdr "include/FBDEV/mali_fbdev_types.h" "$SYSROOT/usr/include/EGL/mali_fbdev_types.h"

# KHR (EGL + GLES 头都 #include <KHR/khrplatform.h>)
fetch_hdr "include/KHR/khrplatform.h"      "$SYSROOT/usr/include/KHR/khrplatform.h"

# GLES2 完整套 (gl2.h:37 #include <GLES2/gl2platform.h>)
fetch_hdr "include/GLES2/gl2.h"            "$SYSROOT/usr/include/GLES2/gl2.h"
fetch_hdr "include/GLES2/gl2ext.h"         "$SYSROOT/usr/include/GLES2/gl2ext.h"
fetch_hdr "include/GLES2/gl2platform.h"    "$SYSROOT/usr/include/GLES2/gl2platform.h"

# GLES1 (向后兼容, blob 导出 gl.h 符号)
fetch_hdr "include/GLES/egl.h"            "$SYSROOT/usr/include/GLES/egl.h"
fetch_hdr "include/GLES/gl.h"             "$SYSROOT/usr/include/GLES/gl.h"
fetch_hdr "include/GLES/glext.h"          "$SYSROOT/usr/include/GLES/glext.h"
fetch_hdr "include/GLES/glplatform.h"     "$SYSROOT/usr/include/GLES/glplatform.h"

# --- 4. 落盘校验: 关键 header 必须存在且非空 ---
log "=== Header install verification ==="
FAIL=0
for must in \
    EGL/egl.h EGL/eglext.h EGL/eglplatform.h EGL/mali_fbdev_types.h \
    KHR/khrplatform.h \
    GLES2/gl2.h GLES2/gl2ext.h GLES2/gl2platform.h
do
    f="$SYSROOT/usr/include/$must"
    if [ -s "$f" ]; then
        log "  OK: $must ($(wc -c < "$f") B)"
    else
        log "  FAIL: $must (missing or empty)"
        FAIL=1
    fi
done
[ "$FAIL" -eq 0 ] || die "FATAL: critical headers missing -- cannot build RetroArch with EGL"

# gbm.h (RA plain_drm/kms 路径可能 include, 无害装上)
fetch_hdr "include/gbm.h" "$SYSROOT/usr/include/gbm.h" || true
# mali.icd (OpenCL vendor 文件, 非必须)
fetch_hdr "include/mali.icd" "$SYSROOT/usr/include/mali.icd" || true

log "=== libmali staged into $SYSROOT/usr/lib ==="
ls -la "$SYSROOT/usr/lib/" | grep -E 'mali|Mali|EGL|GLES' || true
log "DONE. RetroArch can now link -lEGL -lGLESv2 -lmali + find FBDEV headers via -I$SYSROOT/usr/include"
