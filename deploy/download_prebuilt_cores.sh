#!/usr/bin/env bash
# download_prebuilt_cores.sh -- 从 libretro buildbot 下载预编译 ARM 核心
# 目的：节省 CI 时间（核心预编译，无需每次从源码编译）
# 平台：armv7-neon-hf（优选）或 armhf（fallback）
# 用法：download_prebuilt_cores.sh <CORE_OUT_DIR> [CORES_LIST]
#   CORES_LIST 为空 = 下载所有可用核心
# 输出：下载成功解压到 CORE_OUT_DIR，每核输出一行 OK/FAIL
set -euo pipefail

CORE_OUT="${1:?CORE_OUT dir required}"
shift
WANTED=("$@")  # 可选：只想下载的特定核心列表

# Buildbot 的 armv7-neon-hf 平台（ARMv7 + NEON + hard-float，与 RK3036G 匹配）
# 回退到 armhf（通用 ARM hard-float，含更多核心）
NEON_URL="https://buildbot.libretro.com/nightly/linux/armv7-neon-hf/latest"
ARMHF_URL="https://buildbot.libretro.com/nightly/linux/armhf/latest"

mkdir -p "$CORE_OUT"
DOWNLOADED=0
FAILED=0

# 获取 buildbot 可用核心列表（缓存，避免重复请求）
_get_available() {
    local url="$1"
    curl -sSL --max-time 15 "$url" 2>/dev/null \
        | grep -oP '[a-zA-Z0-9_]+_libretro\.so\.zip' \
        | sed 's/_libretro\.so\.zip//' \
        | sort -u
}

echo "[download] Fetching available core list from armv7-neon-hf..."
NEON_CORES=$(_get_available "$NEON_URL/")
echo "[download] armv7-neon-hf: $(echo "$NEON_CORES" | wc -l) cores"

echo "[download] Fetching available core list from armhf (fallback)..."
ARMHF_CORES=$(_get_available "$ARMHF_URL/")
echo "[download] armhf: $(echo "$ARMHF_CORES" | wc -l) cores"

# 合并可用列表（去重）
ALL_AVAILABLE=$(printf '%s\n%s\n' "$NEON_CORES" "$ARMHF_CORES" | sort -u)
echo "[download] total unique: $(echo "$ALL_AVAILABLE" | wc -l) cores"

# 如果没有指定 WANTED，下载所有可用核心
if [ ${#WANTED[@]} -eq 0 ]; then
    TO_DOWNLOAD="$ALL_AVAILABLE"
else
    TO_DOWNLOAD=""
    for c in "${WANTED[@]}"; do
        if echo "$ALL_AVAILABLE" | grep -qx "$c"; then
            TO_DOWNLOAD="${TO_DOWNLOAD:+$TO_DOWNLOAD$'\n'}$c"
        else
            echo "[download] WARN: $c not found in buildbot -- will compile from source"
        fi
    done
fi

echo "[download] === Starting download (cores to try: $(echo "$TO_DOWNLOAD" | wc -l)) ==="

echo "$TO_DOWNLOAD" | while IFS= read -r core; do
    [ -z "$core" ] && continue
    # 已存在则跳过
    if [ -f "$CORE_OUT/${core}_libretro.so" ]; then
        echo "[download]   SKIP $core (already exists)"
        continue
    fi

    # 优先 armv7-neon-hf，回退 armhf
    for platform in "$NEON_URL" "$ARMHF_URL"; do
        url="${platform}/${core}_libretro.so.zip"
        tmpfile="$(mktemp)"
        # 下载（带重试）
        if curl -fsSL --max-time 60 --retry 3 --retry-delay 2 -o "$tmpfile" "$url" 2>/dev/null; then
            # 解压（确保 .so 在顶层目录）
            unzip -q -o -j "$tmpfile" "*_libretro.so" -d "$CORE_OUT/" 2>/dev/null || \
                unzip -q -o -j "$tmpfile" "*.so" -d "$CORE_OUT/" 2>/dev/null || true
            rm -f "$tmpfile"
            if [ -f "$CORE_OUT/${core}_libretro.so" ]; then
                chmod +x "$CORE_OUT/${core}_libretro.so"
                echo "[download]   OK  $core (from ${platform##*/latest/})"
                DOWNLOADED=$((DOWNLOADED+1))
                break
            fi
        fi
        rm -f "$tmpfile"
    done
    if [ ! -f "$CORE_OUT/${core}_libretro.so" ]; then
        echo "[download]   FAIL $core (not in any buildbot platform)"
        FAILED=$((FAILED+1))
    fi
done

echo "[download] === Summary: $DOWNLOADED downloaded, $FAILED failed ==="