#!/bin/bash
# =============================================================================
#  CubeGM -- CNB 构建产物发布脚本 (Release 版本)
#  把 deploy/cubegm/ 打包为 zip 并发布到 CNB Release (stable public download)。
#  在 .cnb.yml 的 build success 后执行。
#
#  下载 URL (公开可下, 无需 token):
#    https://cnb.cool/{org}/{repo}/-/releases/download/{tag}/{zip}
#
#  内部流程 (2026-09-05 实测 API):
#    1. POST /-/releases                      {tag_name,name,body,draft,prerelease,target_commitish}
#    2. POST /{rid}/asset-upload-url          {asset_name,overwrite,size} -> {upload_url, verify_url}
#    3. PUT  upload_url                        (octet-stream 直传)
#    4. POST verify_url                        (带 Content-Type: application/json) 确认
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# 关键修复: CNB CI 中 CNB_REPO_SLUG 实际是完整路径 "lieguch/CubeGM_RetroArch"
# (不是单独 repo slug), 不能再拼 ${CNB_GROUP_SLUG}/${CNB_REPO_SLUG} 否则 URL
# 变成 "lieguch/lieguch/CubeGM_RetroArch" -> 404 Resource not found (stage-2 fail)。
# 用 bash 取最后一段做兜底, 保证最终 REPO 形如 "<org>/<repo>" 且只含一个 "/"。
# 优先级: $REPO 显式 > CNB_REPO_SLUG > CNB_GROUP_SLUG/CNB_REPO_PATH basename > 硬编码
_slug_full="${CNB_REPO_SLUG:-}"
_slug_short="${CNB_REPO_PATH:-${CNB_REPO_SLUG:-CubeGM_RetroArch}}"
_repo_short="$(basename "$_slug_short")"
_org="${CNB_GROUP_SLUG:-lieguch}"
REPO="${REPO:-${_slug_full:-${_org}/${_repo_short}}}"
TOKEN="${CNB_TOKEN:-flUpExezGgRdVv8q1e2205htFsE}"
API="https://api.cnb.cool"

TAG="${TAG:-v7.4e-payload}"
TS=$(date -u +%Y%m%d%H%M)
ZIP="cubegm-payload-${TS}.zip"

echo "== publish: $REPO tag=$TAG asset=$ZIP =="
# 校验: 路径只允许一个 "/", 防止双 org 拼接回归
_slashes=$(echo "$REPO" | tr -cd '/' | wc -c)
if [ "$_slashes" -ne 1 ]; then
  echo "ERROR: REPO path malformed (expected '<org>/<repo>', got '$REPO')"
  exit 1
fi

# 1) 打包（若 cubegm/ 存在）
if [ ! -d "$HERE/cubegm" ]; then
  echo "ERROR: $HERE/cubegm absent — nothing to publish"
  exit 1
fi
cd "$HERE"
du -sh cubegm
zip -qr "/tmp/$ZIP" cubegm
SIZE=$(stat -c%s "/tmp/$ZIP")
echo "packed -> /tmp/$ZIP ($SIZE bytes)"

# 2) 创建或复用 Release (tag)
# CNB API 要求 Accept: application/json, 否则返回 406 导致 json.load 失败
RELEASE_ID=$(curl -s -H "Authorization: Bearer ***" -H "Accept: application/json" "$API/$REPO/-/releases/latest" \
  | python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('id',''))" 2>/dev/null || echo "")
if [ -n "$RELEASE_ID" ] && [ "$RELEASE_ID" != "None" ]; then
  echo "release already exists id=$RELEASE_ID"
else
  CREATE=$(curl -s -X POST -H "Authorization: Bearer ***" -H "Accept: application/json" -H "Content-Type: application/json" \
    -d "{\"tag_name\":\"$TAG\",\"name\":\"CubeGM payload $TAG\",\"body\":\"v7.4e RetroArch audio rewrite build\",\"draft\":false,\"prerelease\":false,\"target_commitish\":\"${CNB_DEFAULT_BRANCH:-main}\"}" \
    "$API/$REPO/-/releases")
  echo "create resp: ${CREATE:0:300}"
  RELEASE_ID=$(echo "$CREATE" | python3 -c "import sys,json;print(json.load(sys.stdin).get('id',''))" 2>/dev/null || echo "")
  if [ -z "$RELEASE_ID" ] || [ "$RELEASE_ID" = "None" ]; then
    echo "ERROR: release create failed: ${CREATE:0:500}"
    exit 1
  fi
  echo "release created id=$RELEASE_ID"
fi

# 3) 申请上传 URL -> {upload_url, verify_url}
UP=$(curl -s -X POST -H "Authorization: Bearer ***" -H "Accept: application/json" -H "Content-Type: application/json" \
  -d "{\"asset_name\":\"$ZIP\",\"overwrite\":true,\"size\":$SIZE}" \
  "$API/$REPO/-/releases/$RELEASE_ID/asset-upload-url")
UPLOAD_URL=$(echo "$UP" | python3 -c "import sys,json;print(json.load(sys.stdin).get('upload_url',''))" 2>/dev/null || echo "")
VERIFY_URL=$(echo "$UP" | python3 -c "import sys,json;print(json.load(sys.stdin).get('verify_url',''))" 2>/dev/null || echo "")
if [ -z "$UPLOAD_URL" ] || [ -z "$VERIFY_URL" ]; then
  echo "ERROR: upload-url resp malformed: ${UP:0:500}"
  exit 1
fi

# 4) PUT 上传
echo "PUT -> ${UPLOAD_URL:0:80}..."
curl -sS -X PUT -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/octet-stream" \
  --data-binary "@/tmp/$ZIP" "$UPLOAD_URL" | head -c 200
echo
echo "PUT done ($SIZE bytes)"

# 5) confirm (必须带 Content-Type: application/json)
curl -sS -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" -d '{}' \
  "$VERIFY_URL" | head -c 200
echo
echo "confirm done"

echo "=============================================================="
echo " DOWNLOAD URL (public): https://cnb.cool/$REPO/-/releases/download/$TAG/$ZIP"
echo "=============================================================="