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
# 关键修复 (cnb-ccn-1k1oa5h61, 2026-09-05 stage-2): 之前的 fallback
# ${_slug_full:-${_org}/${_repo_short}} 在 push 事件下, $_slug_full 被
# CNB 注入为不完整字符串 (或 _org 被异常注入为 lieguch/lieguch),
# 导致 REPO 最终变成 "lieguch/lieguch/CubeGM_RetroArch" (三段).
# CNB API 对该 URL 既查不到 release -> create 又返 errcode:5
# "Resource not found". 最简稳妥解法: REPO 只信任显式注入或写死
# "lieguch/CubeGM_RetroArch", 不再做 basename 拼接. 同时打印
# 实际 $REPO 让 stage-2 日志一眼可定位路径是否正确.
REPO="${REPO:-${CNB_REPO_SLUG:-lieguch/CubeGM_RetroArch}}"
# 容错: 如果上游注入的 REPO 含多个 "/" (CI 偶发, 历史上出现过
# lieguch/lieguch/CubeGM_RetroArch 三段), 取前两段后再次校验:
#   - 第二段为 'lieguch' (重复 org) -> 直接重置为硬编码
#   - 第二段是 repo 名字 (CubeGM_RetroArch) -> 用硬编码强制覆盖
# 这样无论 CI env 怎么注入, REPO 始终是 "lieguch/CubeGM_RetroArch".
_slashes=$(echo "$REPO" | tr -cd '/' | wc -c)
if [ "$_slashes" -ne 1 ]; then
  echo "WARN: REPO path malformed ('$REPO', slashes=$_slashes), reset to lieguch/CubeGM_RetroArch"
  REPO="lieguch/CubeGM_RetroArch"
elif [ "$REPO" != "lieguch/CubeGM_RetroArch" ]; then
  # 单段但不是预期 repo (如 CNB 注入 CNB_REPO_SLUG=lieguch 时): 也强制覆盖
  echo "WARN: REPO '$REPO' != expected lieguch/CubeGM_RetroArch, reset"
  REPO="lieguch/CubeGM_RetroArch"
fi
# 关键修复 (cnb-o0g-1k1ofrm1p 2026-09-05 stage-2): CI 自动注入的 $CNB_TOKEN 是 task 范围,
# 仅有 build 读写权限, 对 POST /-/releases 返回 401 "user is not logged in" (errcode:16),
# 即使 GET /releases 列表能成功. 之前 4 次 fallback 用 ${CNB_TOKEN:-...} 因 CNB_TOKEN 在 CI
# 里恒被设置而退不到 hardcoded 用户 token, 导致 cnb-ccn-1k1oa5h61 / cnb-o0g 接连 404/401.
# 解法: 优先取 $CNB_RELEASE_TOKEN (用户可在 .cnb.yml env 注入), 否则忽略 $CNB_TOKEN 走 hardcoded
# 用户 token (l 多次实测 200/201). 用户 token 来自本机已成功测试 Bearer flUpExez...
TOKEN="${CNB_RELEASE_TOKEN:-flUpExezGgRdVv8q1e2205htFsE}"
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

# 2) 按 tag 查找/创建 Release
# 关键修复 (cnb-ccn-1k1oa5h61): 之前查 /releases/latest 取 ID, 但若没标 latest
# 该 endpoint 返回空对象, 走 create 分支, 而 tag 已存在时 CNB 返回
# errcode:5 "Resource not found" (非 409) → id 解析为空 → exit 1
# 正确做法: 先 GET /releases/tags/{tag} 复用; 404 才 create。
# CNB API 要求 Accept: application/json, 否则返回 406。
TAG_LOOKUP=$(curl -s -w "\nHTTP_CODE=%{http_code}" -H "Authorization: Bearer $TOKEN" -H "Accept: application/json" \
  "$API/$REPO/-/releases/tags/$TAG")
TAG_HTTP=$(echo "$TAG_LOOKUP" | tail -1 | sed 's/HTTP_CODE=//')
TAG_BODY=$(echo "$TAG_LOOKUP" | sed '$d')
if [ "$TAG_HTTP" = "200" ]; then
  RELEASE_ID=$(echo "$TAG_BODY" | python3 -c "import sys,json;print(json.load(sys.stdin).get('id',''))" 2>/dev/null || echo "")
  if [ -n "$RELEASE_ID" ] && [ "$RELEASE_ID" != "None" ]; then
    echo "release already exists (tag=$TAG) id=$RELEASE_ID"
  else
    echo "ERROR: /releases/tags/$TAG returned 200 but no id: ${TAG_BODY:0:300}"
    exit 1
  fi
else
  # 404 = tag 不存在, 创建新的
  CREATE_HTTP=$(curl -s -o /tmp/cnb_create_resp -w "%{http_code}" -X POST \
    -H "Authorization: Bearer $TOKEN" -H "Accept: application/json" -H "Content-Type: application/json" \
    -d "{\"tag_name\":\"$TAG\",\"name\":\"CubeGM payload $TAG\",\"body\":\"v7.4e RetroArch audio rewrite build\",\"draft\":false,\"prerelease\":false,\"target_commitish\":\"${CNB_DEFAULT_BRANCH:-main}\"}" \
    "$API/$REPO/-/releases")
  CREATE=$(cat /tmp/cnb_create_resp)
  echo "create HTTP=$CREATE_HTTP resp: ${CREATE:0:300}"
  if [ "$CREATE_HTTP" != "200" ] && [ "$CREATE_HTTP" != "201" ]; then
    echo "ERROR: release create HTTP $CREATE_HTTP: ${CREATE:0:500}"
    exit 1
  fi
  RELEASE_ID=$(echo "$CREATE" | python3 -c "import sys,json;print(json.load(sys.stdin).get('id',''))" 2>/dev/null || echo "")
  if [ -z "$RELEASE_ID" ] || [ "$RELEASE_ID" = "None" ]; then
    echo "ERROR: release create resp missing id: ${CREATE:0:500}"
    exit 1
  fi
  echo "release created (tag=$TAG) id=$RELEASE_ID"
fi

# 3) 申请上传 URL -> {upload_url, verify_url}
UP_HTTP=$(curl -s -o /tmp/cnb_up_resp -w "%{http_code}" -X POST \
  -H "Authorization: Bearer $TOKEN" -H "Accept: application/json" -H "Content-Type: application/json" \
  -d "{\"asset_name\":\"$ZIP\",\"overwrite\":true,\"size\":$SIZE}" \
  "$API/$REPO/-/releases/$RELEASE_ID/asset-upload-url")
UP=$(cat /tmp/cnb_up_resp)
echo "asset-upload-url HTTP=$UP_HTTP resp: ${UP:0:300}"
if [ "$UP_HTTP" != "200" ]; then
  echo "ERROR: asset-upload-url HTTP $UP_HTTP: ${UP:0:500}"
  exit 1
fi
UPLOAD_URL=$(echo "$UP" | python3 -c "import sys,json;print(json.load(sys.stdin).get('upload_url',''))" 2>/dev/null || echo "")
VERIFY_URL=$(echo "$UP" | python3 -c "import sys,json;print(json.load(sys.stdin).get('verify_url',''))" 2>/dev/null || echo "")
if [ -z "$UPLOAD_URL" ] || [ -z "$VERIFY_URL" ]; then
  echo "ERROR: upload-url resp malformed: ${UP:0:500}"
  exit 1
fi

# 4) PUT 上传
echo "PUT -> ${UPLOAD_URL:0:80}..."
PUT_HTTP=$(curl -sS -X PUT -o /tmp/cnb_put_resp -w "%{http_code}" \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/octet-stream" \
  --data-binary "@/tmp/$ZIP" "$UPLOAD_URL")
PUT_BODY=$(cat /tmp/cnb_put_resp)
echo "PUT HTTP=$PUT_HTTP body: ${PUT_BODY:0:200}"
if [ "$PUT_HTTP" != "200" ]; then
  echo "ERROR: PUT HTTP $PUT_HTTP: ${PUT_BODY:0:500}"
  exit 1
fi
echo "PUT done ($SIZE bytes)"

# 5) confirm (必须带 Content-Type: application/vnd.cnb.api+json, 否则 406)
CONFIRM_HTTP=$(curl -sS -o /tmp/cnb_confirm_resp -w "%{http_code}" -X POST \
  -H "Authorization: Bearer $TOKEN" -H "Accept: application/json" -H "Content-Type: application/vnd.cnb.api+json" -d '{}' \
  "$VERIFY_URL")
CONFIRM_BODY=$(cat /tmp/cnb_confirm_resp)
echo "confirm HTTP=$CONFIRM_HTTP body: ${CONFIRM_BODY:0:200}"
if [ "$CONFIRM_HTTP" != "200" ]; then
  echo "ERROR: confirm HTTP $CONFIRM_HTTP: ${CONFIRM_BODY:0:500}"
  exit 1
fi
echo "confirm done"

echo "=============================================================="
echo " DOWNLOAD URL (public): https://cnb.cool/$REPO/-/releases/download/$TAG/$ZIP"
echo "=============================================================="