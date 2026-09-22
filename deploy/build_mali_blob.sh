#!/usr/bin/env bash
# =============================================================================
#  build_mali_blob.sh -- Install libmali-rk-utgard-400 gbm (DRM) blob + headers into sysroot
#  (v3: GBM/DRM 变体, 替代 fbdev 变体 — 铁证: 设备显示栈是 DRM(/dev/dri/card0+renderD128),
#   fbdev 变体走 /dev/fb0 framebuffer panning 拿不到 framebuffer → eglGetDisplay EGL_NO_DISPLAY。
#   gbm 变体 VARIANT=...-drm-dma_buf, open /dev/dri/card0+/dev/mali, 自带 libgbm, 无 libdrm 依赖)
# =============================================================================
set -euo pipefail

SYSROOT="${SYSROOT:?SYSROOT must be set}"
MALI_REPO="https://github.com/paolosabatino/libmali-rk-utgard-400"
BLOB_NAME="libmali-utgard-400-r7p0-r0p0-gbm.so"
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
    log "Downloading libmali gbm (DRM) blob via GitHub API..."
    curl -s -m 120 "${API_BASE}/lib/arm-linux-gnueabihf/${BLOB_NAME}" \
        | python3 -c "import sys,json,base64; d=json.load(sys.stdin); open('${BLOB_DST}','wb').write(base64.b64decode(d['content']))" \
        || die "blob download via GitHub API failed"
    [ -s "$BLOB_DST" ] || die "blob file is empty after download"
    log "blob: $(wc -c < "$BLOB_DST") bytes"
fi

# --- 2. Symlinks ---
# gbm 变体同时提供 EGL + GLESv2 + GLESv1 + GBM 全套符号 (readelf 实测)。
# RetroArch --enable-kms 的 check_val link -lgbm -ldrm:
#   -lgbm  -> 由本 blob 提供 gbm_create_device 等 (symlink libgbm.so 指向 blob)
#   -ldrm  -> 由独立 libdrm (STAGE 4.9 从 Debian armhf deb 提取) 提供
log "Creating symlinks..."
cd "$SYSROOT/usr/lib"
ln -sf "$BLOB_NAME" libEGL.so
ln -sf "$BLOB_NAME" libEGL.so.1
ln -sf "$BLOB_NAME" libGLESv2.so
ln -sf "$BLOB_NAME" libGLESv2.so.2
ln -sf "$BLOB_NAME" libGLESv1_CM.so
ln -sf "$BLOB_NAME" libGLESv1_CM.so.1
ln -sf "$BLOB_NAME" libgbm.so
ln -sf "$BLOB_NAME" libgbm.so.1
ln -sf "$BLOB_NAME" libMali.so
ln -sf "$BLOB_NAME" libmali.so
cd - >/dev/null

# --- 3. Download headers (GBM 变体用根 EGL 头 + gbm.h) ---
# 关键: 根 include/EGL/eglplatform.h 含 __GBM__ 分支 (typedef gbm_device*);
# gbm.h 自身 #define __GBM__ 1。fbdev 版用的是 include/FBDEV/ 子目录 (mali_fbdev_types.h),
# gbm 版改用根目录, 不再要 mali_fbdev_types.h。
log "Installing headers (GBM EGL + KHR + GLES2 + gbm.h)..."
fetch_hdr() {
    local rel="$1" dest="$2"
    [ -s "$dest" ] && return 0
    mkdir -p "$(dirname "$dest")"
    curl -s -m 60 "${API_BASE}/${rel}" \
        | python3 -c "import sys,json,base64; d=json.load(sys.stdin); open('${dest}','wb').write(base64.b64decode(d['content']))" \
        || { log "WARN: failed: $rel"; return 1; }
    [ -s "$dest" ] && log "  $rel -> $(wc -c < "$dest") bytes"
}

# GBM EGL headers (根目录, 走 __GBM__ 分支, 避免 X11 依赖)
fetch_hdr "include/EGL/egl.h"             "$SYSROOT/usr/include/EGL/egl.h"
fetch_hdr "include/EGL/eglext.h"         "$SYSROOT/usr/include/EGL/eglext.h"
fetch_hdr "include/EGL/eglplatform.h"     "$SYSROOT/usr/include/EGL/eglplatform.h"

# KHR (EGL + GLES 头都 #include <KHR/khrplatform.h>)
fetch_hdr "include/KHR/khrplatform.h"      "$SYSROOT/usr/include/KHR/khrplatform.h"

# GLES2 完整套
fetch_hdr "include/GLES2/gl2.h"            "$SYSROOT/usr/include/GLES2/gl2.h"
fetch_hdr "include/GLES2/gl2ext.h"         "$SYSROOT/usr/include/GLES2/gl2ext.h"
fetch_hdr "include/GLES2/gl2platform.h"    "$SYSROOT/usr/include/GLES2/gl2platform.h"

# GLES1 (向后兼容, blob 导出 gl.h 符号)
fetch_hdr "include/GLES/egl.h"            "$SYSROOT/usr/include/GLES/egl.h"
fetch_hdr "include/GLES/gl.h"             "$SYSROOT/usr/include/GLES/gl.h"
fetch_hdr "include/GLES/glext.h"          "$SYSROOT/usr/include/GLES/glext.h"
fetch_hdr "include/GLES/glplatform.h"     "$SYSROOT/usr/include/GLES/glplatform.h"

# gbm.h (RetroArch drm_ctx.c 必需, 定义 __GBM__ 宏 + gbm_* 声明)
fetch_hdr "include/gbm.h"                 "$SYSROOT/usr/include/gbm.h"

# --- 4. 落盘校验: 关键 header 必须存在且非空 ---
log "=== Header install verification ==="
FAIL=0
for must in \
    EGL/egl.h EGL/eglext.h EGL/eglplatform.h \
    KHR/khrplatform.h \
    GLES2/gl2.h GLES2/gl2ext.h GLES2/gl2platform.h \
    gbm.h
do
    f="$SYSROOT/usr/include/$must"
    if [ -s "$f" ]; then
        log "  OK: $must ($(wc -c < "$f") B)"
    else
        log "  FAIL: $must (missing or empty)"
        FAIL=1
    fi
done
[ "$FAIL" -eq 0 ] || die "FATAL: critical headers missing -- cannot build RetroArch with GBM/KMS"

log "=== libmali (gbm/drm) staged into $SYSROOT/usr/lib ==="
ls -la "$SYSROOT/usr/lib/" | grep -E 'mali|Mali|EGL|GLES|gbm' || true
log "DONE. RetroArch can now link -lEGL -lGLESv2 -lgbm + find GBM headers via -I$SYSROOT/usr/include"