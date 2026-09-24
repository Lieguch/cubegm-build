#!/usr/bin/env bash
# =============================================================================
#  build_mali_blob.sh -- Install libmali-rk-utgard-400 gbm (DRM) blob + headers into sysroot
#
#  (v3: GBM/DRM 变体, 替代 fbdev 变体 — 铁证: 设备显示栈是 DRM(/dev/dri/card0+renderD128,
#   fbs=0), fbdev 变体走 /dev/fb0 framebuffer panning 拿不到 framebuffer
#   → eglGetDisplay 返回 EGL_NO_DISPLAY。gbm 变体 VARIANT=...-drm-dma_buf,
#   open /dev/dri/card0+/dev/mali, 自带完整 gbm_* 符号)
#
#  (v4: 只换 blob —— 修复 run 512 的 `-lEGL ... no` 失败)
#
#   run 512 根因 (实证, 非推测):
#     paolosabatino 的 r0p0-gbm.so 带 6 个「无版本标签」的 GLOBAL UND 符号:
#        BN_bin2bn BN_new BN_set_word RSA_new RSA_public_decrypt RSA_size  (OpenSSL)
#     它自己的 .gnu.version_r 只声明 6 个库 (libc/libm/librt/libdl/libpthread/ld),
#     没有 libcrypto → 这 6 个符号在链接期无解。
#
#     RetroArch qb/qb.libs.sh 的 check_lib() 做链接测试:
#        $(COMPILER) -o $TEMP_EXE $TEMP_CODE $BUILD_DIRS $FLAGS $LDFLAGS -lEGL
#     GNU ld 对「被保留且被引用」的共享库会递归校验其 GLOBAL UND 符号。
#     本地最小复现 (Ubuntu gcc 默认带 --as-needed):
#        不引用库符号 → --as-needed 丢弃整库 → exit=0
#        引用 eglGetDisplay → 库被保留 → `undefined reference to 'BN_new'` → exit=1
#     EGL 符号当然被引用 ⇒ 库必被保留 ⇒ 必然报错 ⇒ 判定 no
#     ⇒ die "Forced to build with library -lEGL, but cannot locate. Exiting ..."
#     (该 check 第 8 参为 true, 属 critical, 失败即退出)
#
#     tsukumijima 的 r1p1-gbm.so 实测: 114/114 GLOBAL UND 全带 glibc 版本标签,
#     零外部库依赖。EGL 符号集与 r0p0 逐符号完全一致 (57 个, 零差异),
#     且同样 open("/dev/dri/card0") + open("/dev/mali"),
#     VARIANT=...-drm-dma_buf → 头文件无需改动 (gbm.h 的 __GBM__ 路径照用)。
#
#   ★ 头文件仍全部取自 paolosabatino (#511 已证明可用):
#     - paolosabatino 有 include/KHR/khrplatform.h + 根 include/gbm.h
#     - tsukumijima 的 KHR 只有 mali_khrplatform.h, 且 gbm.h 在 include/GBM/<ver>/
#       → 换源会连带 KHR 路径失效, 没必要。
#     两仓库 include/EGL/eglplatform.h 的 __GBM__ 分支逐字一致 (已比对)。
# =============================================================================
set -euo pipefail

SYSROOT="${SYSROOT:?SYSROOT must be set}"

# 头文件源 (与 run 511 相同, 已验证)
HDR_RAW="https://raw.githubusercontent.com/paolosabatino/libmali-rk-utgard-400/master"
HDR_API="https://api.github.com/repos/paolosabatino/libmali-rk-utgard-400/contents"
# blob 源 (换源: r1p1 无外部库依赖)
BLOB_RAW="https://raw.githubusercontent.com/tsukumijima/libmali-rockchip/master"
BLOB_API="https://api.github.com/repos/tsukumijima/libmali-rockchip/contents"

# r1p1-gbm: 941540 B
#   sha256 a4080756b7e0ab157aaf2a5281287ea56038c14b45aac6a7308ccd1aa9d332fa
BLOB_NAME="libmali-utgard-400-r7p0-r1p1-gbm.so"
BLOB_REL="lib/arm-linux-gnueabihf/${BLOB_NAME}"

# -----------------------------------------------------------------------------
# 下载助手: 优先 raw.githubusercontent (无速率限制), 回退 GitHub API base64。
#   匿名 API 限 60 次/小时/IP —— 本脚本要取 1 blob + 12 头文件 = 13 次,
#   加上 CI 里其他 API 调用很容易撞 403 rate limit (实测踩过),
#   而 raw 不计入 API 配额, 也不会遇到 GitHub 弃用 base64 content 字段的风险。
# -----------------------------------------------------------------------------
fetch_url() {
    # $1 = raw URL, $2 = api contents URL, $3 = 目标文件
    local raw="$1" api="$2" dest="$3"
    if curl -fsSL -m 120 -o "$dest" "$raw" 2>/dev/null && [ -s "$dest" ]; then
        return 0
    fi
    log "  raw fetch failed, falling back to GitHub API: $(basename "$dest")"
    curl -s -m 120 "$api" \
        | python3 -c "import sys,json,base64; d=json.load(sys.stdin); sys.exit(1) if 'content' not in d else open('${dest}','wb').write(base64.b64decode(d['content']))" \
        && [ -s "$dest" ]
}

# 回退候选 (仅当设备 diag 报 "Device driver API mismatch" 时才考虑换版本):
#   paolosabatino libmali-utgard-400-r7p0-r0p0-gbm.so (839→851KB)
#   ⚠ 该 blob 带上述 6 个 OpenSSL 未定义符号，换回时必须同时提供 libcrypto
#     或加 -Wl,--allow-shlib-undefined，否则 -lEGL 链接测试必然失败 (run 512 实证)。
#   tsukumijima libmali-utgard-400-r7p0-r3p0-wayland-gbm.so (非 DRM 变体, 不适用)

log(){ printf '\033[1;34m[mali]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

[ -d "$SYSROOT/usr" ] || die "SYSROOT $SYSROOT/usr missing"
mkdir -p "$SYSROOT/usr/lib" \
    "$SYSROOT/usr/include/EGL" \
    "$SYSROOT/usr/include/GLES2" \
    "$SYSROOT/usr/include/GLES" \
    "$SYSROOT/usr/include/KHR"

# --- 1. Download blob (raw 优先, API 回退) ---
BLOB_DST="$SYSROOT/usr/lib/$BLOB_NAME"
if [ -f "$BLOB_DST" ] && [ -s "$BLOB_DST" ]; then
    log "libmali blob already in sysroot -- skip"
else
    log "Downloading libmali gbm (DRM) blob..."
    fetch_url "${BLOB_RAW}/${BLOB_REL}" "${BLOB_API}/${BLOB_REL}" "$BLOB_DST" \
        || die "blob download failed (raw + API both failed)"
    [ -s "$BLOB_DST" ] || die "blob file is empty after download"
    log "blob: $(wc -c < "$BLOB_DST") bytes"
    # 校验: 换源后 blob 内容必须与实证分析的那一份完全一致
    BLOB_SHA="$(sha256sum "$BLOB_DST" | awk '{print $1}')"
    if [ "$BLOB_SHA" != "a4080756b7e0ab157aaf2a5281287ea56038c14b45aac6a7308ccd1aa9d332fa" ]; then
        log "  WARN: blob sha256 = $BLOB_SHA"
        log "  WARN: expected        a4080756b7e0ab157aaf2a5281287ea56038c14b45aac6a7308ccd1aa9d332fa"
        log "  WARN: 上游可能改了文件 -- 下面第 1b 步的门禁会独立把关, 不因哈希不符而中止"
    else
        log "  sha256 OK"
    fi
fi

# -----------------------------------------------------------------------------
# 1b. ★ 预检门禁: blob 必须零外部库依赖  (run 512 根因固化为构建期断言)
#
#   判据: 带 @GLIBC_x 版本标签的 UND 符号必由 glibc(familiy) 提供；
#         无版本标签的 GLOBAL UND 符号 = 外部库依赖 = -lEGL 链接测试必败。
#   未来换 blob 若回归, 这里立刻红牌, 而不是等 RetroArch configure
#   报一句无从下手的 "cannot locate"。
# -----------------------------------------------------------------------------
log "=== Blob dependency gate (GLOBAL UND symbols must all be glibc-versioned) ==="
READELF_BIN="${CROSS_COMPILE:-}readelf"
command -v "$READELF_BIN" >/dev/null 2>&1 || READELF_BIN="readelf"

UND_ALL="$(  "$READELF_BIN" --dyn-syms -W "$BLOB_DST" 2>/dev/null \
           | awk '$7=="UND" && $5=="GLOBAL" {print $8}' )"
UND_UNTAGGED="$(printf '%s\n' "$UND_ALL" | grep -v '@' | grep -v '^$' | sort -u || true)"

if [ -n "$UND_UNTAGGED" ]; then
    log "  X blob has $(printf '%s\n' "$UND_UNTAGGED" | wc -l) unversioned GLOBAL UND symbol(s):"
    printf '%s\n' "$UND_UNTAGGED" | sed 's/^/      /' >&2
    die "libmali blob has external library dependencies -- RetroArch's -lEGL link test WILL fail (see run 512). Pick a blob with zero unversioned UND symbols."
fi
log "  OK: all GLOBAL UND symbols carry glibc version tags -> no external lib deps"

# 必备符号自检 (RetroArch drm_ctx.c / egl_common.c 真正调用的)
# ⚠ 不要写成 `readelf ... | grep -q`：脚本开了 set -o pipefail，grep -q 命中即
#   退出会给 readelf 发 SIGPIPE(141)，pipefail 令整条管道非零 —— 符号明明存在
#   也会误判 (已本地复现)。用 grep -c 读完全部输入。
BLOB_SYMS="$("$READELF_BIN" --dyn-syms -W "$BLOB_DST" 2>/dev/null || true)"
for sym in eglGetDisplay eglInitialize eglChooseConfig eglCreateContext \
           eglCreateWindowSurface eglMakeCurrent eglSwapBuffers gbm_create_device; do
    if [ "$(printf '%s\n' "$BLOB_SYMS" | grep -cE " $sym\$" || true)" -eq 0 ]; then
        die "blob missing required symbol: $sym"
    fi
done
log "  OK: EGL + gbm required symbols present"

# --- 2. Symlinks ---
# gbm 变体同时提供 EGL + GLESv2 + GLESv1 + GBM 全套符号 (readelf 实测)。
# RetroArch --enable-kms 的 check_val link -lgbm -ldrm:
#   -lgbm  -> 由本 blob 提供 gbm_create_device 等 (symlink libgbm.so 指向 blob)
#   -ldrm  -> 由独立 libdrm (STAGE 4.9 从 Debian armhf deb 提取) 提供
#
# ★ SONAME (run 512 后新增): r1p1 blob 的 DT_SONAME = libmali.so.1
#   (r0p0 无 SONAME)。链接时 -lEGL 记的是 SONAME，运行时动态链接器会去找
#   libmali.so.1 → 必须补这个链接名，否则设备上 "cannot open shared object
#   file: libmali.so.1"，屏幕不亮。
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
ln -sf "$BLOB_NAME" libmali.so.1          # <- blob 的 DT_SONAME
cd - >/dev/null

# --- 3. Download headers (GBM 变体用根 EGL 头 + gbm.h) ---
# 关键: 根 include/EGL/eglplatform.h 含 __GBM__ 分支 (typedef gbm_device*);
# gbm.h 自身 #define __GBM__ 1。fbdev 版用的是 include/FBDEV/ 子目录
# (mali_fbdev_types.h), gbm 版改用根目录, 不再要 mali_fbdev_types.h。
#
# ★ run 513 实证的坑: 旧的 fetch_hdr 只要目标文件存在就跳过 ("already in sysroot"),
#   CLI 缓存的 sysroot 里留着上一版 **FBDEV** 头, 于是 "装好了" 其实是错的 ——
#   实测 egl.h=15361 B (= include/FBDEV/egl.h), eglplatform.h=3827 B (= FBDEV 版),
#   而 GBM 版应分别是 20345 B / 5913 B (已逐个下载比对)。
#
#   判据 (实证, 非猜测): GBM 版 eglplatform.h 含 `#elif defined(__GBM__)` 且把
#     EGLNativeDisplayType 定义为 `struct gbm_device *`;
#   FBDEV 版 (3827 B) **完全没有** __GBM__ 分支 (grep 零命中)。
#   用这个指纹决定要不要强制重下, 既避免缓存毒化, 又不会每次构建都重复下载。
log "Installing headers (GBM EGL + KHR + GLES2 + gbm.h)..."
HDR_FP="$SYSROOT/usr/include/EGL/eglplatform.h"
if [ -s "$HDR_FP" ] && grep -q '__GBM__' "$HDR_FP" && grep -q 'gbm_device' "$HDR_FP"; then
    HDR_REFRESH=0
    log "  headers already GBM-flavoured (eglplatform.h has __GBM__ -> gbm_device) -- skip downloads"
else
    HDR_REFRESH=1
    if [ -s "$HDR_FP" ]; then
        log "  headers are FBDEV-flavoured (stale cache: $(wc -c < "$HDR_FP") B, no __GBM__) -- force refresh"
    else
        log "  headers absent -- downloading"
    fi
fi

fetch_hdr() {
    local rel="$1" dest="$2"
    # 仅当「指纹显示已是 GBM 版」且目标存在时才跳过; 否则强制重下 (覆盖缓存毒品)
    [ "${HDR_REFRESH:-1}" -eq 0 ] && [ -s "$dest" ] && return 0
    mkdir -p "$(dirname "$dest")"
    fetch_url "${HDR_RAW}/${rel}" "${HDR_API}/${rel}" "$dest" \
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

# GLES1 (向后兼容, blob 导出 GLES1 符号)
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

# --- 4b. ★ 变体指纹门禁 (run 513 根因固化) ---
#   光"存在且非空"不够: FBDEV 版的 EGL 头同样非空, 但**没有 __GBM__ 分支**,
#   会让 eglplatform.h 走错分支 → KMS/GBM 编译必挂。
#   要求: ① eglplatform.h 必须有 __GBM__ 分支且 EGLNativeDisplayType 是 gbm_device*
#         ② gbm.h 必须 #define __GBM__ (否则 eglplatform.h 拿不到该宏)
log "=== Header variant gate (must be GBM/DRM, not FBDEV) ==="
_GBM_PLAT="$SYSROOT/usr/include/EGL/eglplatform.h"
if ! grep -q '__GBM__' "$_GBM_PLAT"; then
    log "  FAIL: EGL/eglplatform.h has no __GBM__ branch -> this is the FBDEV header"
    log "        ($_GBM_PLAT, $(wc -c < "$_GBM_PLAT") B; GBM 版应含 __GBM__ 且约 5913 B)"
    die "wrong header variant: FBDEV EGL headers cannot build the GBM/KMS context"
fi
if ! grep -q 'gbm_device' "$_GBM_PLAT"; then
    log "  FAIL: EGL/eglplatform.h has __GBM__ but no gbm_device typedef"
    die "EGL/eglplatform.h __GBM__ branch is not the expected gbm_device form"
fi
if ! grep -q 'define __GBM__' "$SYSROOT/usr/include/gbm.h"; then
    log "  FAIL: gbm.h does not #define __GBM__ -> eglplatform.h cannot take the GBM branch"
    die "gbm.h missing '__GBM__' definition"
fi
log "  OK: GBM variant confirmed (eglplatform.h __GBM__ -> gbm_device; gbm.h defines __GBM__)"

# --- 5. 头文件自洽性验证: 用 __GBM__ 路径实编 EGL+GLES2+gbm+DRM 头 ---
# 历史教训: 通用 EGL 头在 Linux 上会走 X11 分支 (#include <X11/Xlib.h>) 必挂;
# 而 RetroArch 的 check_header() 只吃 $CFLAGS, 不吃 INCLUDES —— 头不装好,
# 报错会出现在很远的 configure 阶段。用 -fsyntax-only 就地拦下。
#
# ★ run 513 扩展 (DRM/KMS 头): --enable-kms 启用后 RetroArch 会编
#   gfx/common/drm_common.h (#include <xf86drm.h> / <xf86drmMode.h>) 等文件,
#   而这些头由 build.sh STAGE 4 装进 sysroot。此前它不在这里受检, 于是 run 513
#   一路跑到 STAGE 8 (约 40 分钟后) 才在 Makefile:268 炸
#   "xf86drm.h: No such file or directory"。现在把同一批头纳入就地自检:
#   顺序 —— build.sh 先跑 STAGE 4 (装 DRM 头), 再调本脚本, 所以此刻头已就位。
#
# ⚠ 只在「确认存在 ARM 交叉编译器」时才跑: build.sh 第 99 行
#   export CROSS_COMPILE="${TARGET}-" 会传进来。若退回主机 gcc, -mfloat-abi=hard
#   这类 ARM 专用参数会被 gcc 拒绝 (unrecognized command-line option), 造成
#   本可正常构建却在此误杀 (已本地复现)。所以 CROSS_COMPILE 为空直接跳过。
if [ -n "${CROSS_COMPILE:-}" ] && command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    TMPC="/tmp/mali_hdrchk_$$.c"
    HDRCHK_ERR="/tmp/mali_hdrchk_$$.err"
    cat > "$TMPC" <<'EOF'
/* ⚠ 顺序有意义: gbm.h 必须排在 EGL 头之前 —— 它 #define __GBM__ 1, 而
   EGL/eglplatform.h 靠 __GBM__ 才能走 gbm 分支 (否则落到 X11 分支要 <X11/Xlib.h>)。
   RetroArch 真实源码也是这个顺序: gfx/common/egl_common.h (gbm.h@21 → EGL/egl.h@23)、
   gfx/drivers_context/drm_ctx.c (<libdrm/drm.h> → <gbm.h>)。把 EGL 头放前面会得到
   假失败 "X11/Xlib.h: No such file" (已本地复现)。 */
#include <libdrm/drm.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
/* DRM/KMS context heads (RetroArch --enable-kms) -- run 513 缺这几个头挂了 40 分钟 */
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/drm_fourcc.h>
#include <drm/drm_fourcc.h>
#include <drm_fourcc.h>
int main(void) { return 0; }
EOF
    # 参数与 build.sh 第 33 行 ARCH_FLAGS 保持一致, 保证检查环境 == 真实编译环境
    # ★ -DEGL_NO_X11 必须带: 与 build.sh 全局 CFLAGS 一致 (run 514 根因见 build.sh 注释)。
    #   否则 egl.h 会落到 X11 分支, 这里就会报假失败 "X11/Xlib.h: No such file" ——
    #   与 RetroArch configure 的 check_header '' EGL EGL/egl.h EGL/eglext.h 完全同因。
    if "${CROSS_COMPILE}gcc" -fsyntax-only \
         -march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
         -DEGL_NO_X11 -DMESA_EGL_NO_X11_HEADERS \
         --sysroot="$SYSROOT" -I"$SYSROOT/usr/include" \
         -I"$SYSROOT/usr/include/libdrm" "$TMPC" 2>"$HDRCHK_ERR"; then
        log "  OK: EGL+GLES2+gbm+DRM/KMS headers compile via __GBM__ path (no X11)"
    else
        log "  FAIL: header self-check failed (${CROSS_COMPILE}gcc):"
        sed 's/^/      /' "$HDRCHK_ERR" >&2 || true
        rm -f "$TMPC" "$HDRCHK_ERR"
        die "headers not self-consistent (X11/KHR leak? missing DRM header?) -- see errors above"
    fi
    rm -f "$TMPC" "$HDRCHK_ERR"
else
    log "  SKIP: no ARM cross compiler (CROSS_COMPILE='${CROSS_COMPILE:-}') -- header self-check skipped"
fi

log "=== libmali (gbm/drm) staged into $SYSROOT/usr/lib ==="
ls -la "$SYSROOT/usr/lib/" | grep -E 'mali|Mali|EGL|GLES|gbm' || true
log "DONE. RetroArch can now link -lEGL -lGLESv2 -lgbm + find GBM headers via -I$SYSROOT/usr/include"
