#!/usr/bin/env bash
# 发行脚本：把「已构建并已发布到 OSS 的产物」发布为 GitHub + Gitee 的 Release
#
# 用法：
#   tools/release/publish_release.sh                    # 正式发布
#   DRY_RUN=true tools/release/publish_release.sh       # 只打印将执行的动作
#   SKIP_GITEE=true tools/release/publish_release.sh    # 只发 GitHub
#   SKIP_TAG_PUSH=true tools/release/publish_release.sh # 不 push tag（tag 已在远端）
#   ALLOW_MISSING_ATTACHMENTS=true ...                  # 单平台发版时跳过缺失的另一个平台附件
#   REQUIRE_GITEE=true ...                              # 缺 Gitee token 直接失败（CI 用，避免假成功）
#
# 附件来源：优先用本地 dist/；缺失时自动从 https://dl.xiaotu.online/<product>/ 回源
# （v2 发布名与 OSS 对象名一致），也可用 OSS_BASE=... 覆盖下载域。
#
# 凭据：
#   GitHub  使用 gh CLI（本机 gh auth login，或 CI 里设 GH_TOKEN）
#   Gitee   环境变量 GITEE_TOKEN，或 ~/.config/release/gitee.env 中的 GITEE_TOKEN=（权限 600）
#           CI 里缺 token 时可用 REQUIRE_GITEE=true 让脚本直接失败，避免「只发了 GitHub」的假成功
#
# 跨平台（macOS 本机 / CI 的 Windows Git Bash 均可直接执行）：
#   - 优先 python3，Windows 上 python3 是 Store 占位程序时自动退回 python
#   - 文件大小、SHA256 不依赖 BSD 的 stat -f / shasum
#
# 幂等：
#   - tag 已存在则复用，不覆盖
#   - Release 已存在则更新标题/说明，缺失附件补传，已存在同名附件跳过
#   - 任一平台失败不回滚另一平台，重新执行即可补齐
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

# Windows Git Bash 里 python3 可能是 Microsoft Store 占位程序：真跑一下再决定
PY=""
for cand in python3 python; do
  if command -v "$cand" >/dev/null 2>&1 && "$cand" -c 'print(1)' >/dev/null 2>&1; then
    PY="$cand"; break
  fi
done
[ -n "$PY" ] || { echo "需要可用的 Python 3（python3 或 python）" >&2; exit 2; }

file_size() { wc -c < "$1" | tr -d ' \t\r\n'; }
sha256_hex() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | cut -d' ' -f1
  else
    "$PY" -c 'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"
  fi
}

# ===== 按仓库自动识别配置（三仓共用同一份脚本）=====
PRODUCT_KEY="$(basename "$REPO_ROOT")"
case "$PRODUCT_KEY" in
  xiaotu-inventory)
    PRODUCT_NAME="小兔进销存"
    GH_REPO="LinYuwei82/xiaotu-inventory"
    GITEE_OWNER="lin-yuwei168"; GITEE_REPO="xiaotu-inventory"
    OSS_DIR="xiaotu"
    VERSION_FILE="VERSION.txt"
    ATT_SRC=(
      "dist/xiaotu-inventory-{V}.dmg"
      "dist/xiaotu-inventory-{V}-win64-setup.exe"
      "dist/xiaotu-inventory-{V}-win64.zip"
    )
    ATT_DST=(
      "xiaotu-{V}-macos-arm64.dmg"
      "xiaotu-{V}-win64-setup.exe"
      "xiaotu-{V}-win64-portable.zip"
    )
    ;;
  mote|mote-md)
    PRODUCT_NAME="Mote"
    GH_REPO="LinYuwei82/mote-md"
    GITEE_OWNER="lin-yuwei168"; GITEE_REPO="mote-md"
    OSS_DIR="mote"
    VERSION_FILE="VERSION.txt"
    ATT_SRC=(
      "dist/Mote-macOS-native-{V}.dmg"
      "dist/Mote-Setup-{V}.exe"
      "dist/Mote-Windows.zip"
    )
    ATT_DST=(
      "mote-{V}-macos-arm64.dmg"
      "mote-{V}-win64-setup.exe"
      "mote-{V}-win64-portable.zip"
    )
    ;;
  dev_card_cpp)
    PRODUCT_NAME="DevCard"
    GH_REPO="LinYuwei82/dev_card_cpp"
    GITEE_OWNER="lin-yuwei168"; GITEE_REPO="dev_card_cpp"
    OSS_DIR="devcard"
    VERSION_FILE="VERSION.txt"
    ATT_SRC=(
      "dist/DevCard-{V}-macos-arm64.dmg"
      "dist/DevCard-{V}-setup.exe"
      "dist/DevCard-{V}-win64-portable.zip"
    )
    ATT_DST=(
      "devcard-{V}-macos-arm64.dmg"
      "devcard-{V}-win64-setup.exe"
      "devcard-{V}-win64-portable.zip"
    )
    ;;
  *)
    echo "未识别的仓库目录：${PRODUCT_KEY}（脚本只支持 xiaotu-inventory / mote / dev_card_cpp）" >&2
    exit 2 ;;
esac

VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
[ -n "$VERSION" ] || { echo "无法从 $VERSION_FILE 读取版本号" >&2; exit 1; }
if [ -z "${TAG:-}" ]; then
  # 发布号以 tag 为准：取当前版本最新的 v<semver>+<seq>
  TAG="$(git tag -l "v${VERSION}+*" --sort=-v:refname | head -1)"
  [ -n "$TAG" ] || {
    echo "找不到 v${VERSION}+* tag：请先执行 tools/release/release_tag.py create --push，或显式传 TAG=..." >&2
    exit 2; }
fi
TITLE="${TITLE:-$PRODUCT_NAME $VERSION}"
NOTES="${NOTES_FILE:-docs/release-notes-$VERSION.md}"
DRY_RUN="${DRY_RUN:-false}"
SKIP_GITEE="${SKIP_GITEE:-false}"
SKIP_TAG_PUSH="${SKIP_TAG_PUSH:-false}"
BRANCH="${BRANCH:-$(git rev-parse --abbrev-ref HEAD)}"
# CI 按 tag 检出时是 detached HEAD；Gitee 的 target_commitish 需要一个真实分支
[ "$BRANCH" = "HEAD" ] && BRANCH="main"
[ -f "$NOTES" ] || { echo "发行说明不存在：$NOTES" >&2; exit 1; }

echo "===== 发行 · $PRODUCT_NAME ${VERSION}（tag ${TAG}）====="
echo "仓库：$REPO_ROOT   分支：$BRANCH"
echo "说明：$NOTES"

# ---- ① 收集附件 + 生成 SHA256SUMS.txt ----
STAGE="$(mktemp -d)"
# 标题单独落成 UTF-8 文件：Windows Git Bash 把中文当命令行参数传给 gh/curl 会变成问号
TITLE_FILE="$(mktemp)"
printf '%s' "$TITLE" > "$TITLE_FILE"
# Windows/MSYS 的 /tmp 路径原生 curl 读不到，转成 Windows 路径后传参
TITLE_ARG="$TITLE_FILE"
if command -v cygpath >/dev/null 2>&1; then TITLE_ARG="$(cygpath -w "$TITLE_FILE")"; fi
trap 'rm -rf "$STAGE" "$TITLE_FILE"' EXIT
FILES=()
OSS_BASE="${OSS_BASE:-https://dl.xiaotu.online}"
for i in "${!ATT_SRC[@]}"; do
  src="${ATT_SRC[$i]//\{V\}/$VERSION}"
  dst="${ATT_DST[$i]//\{V\}/$VERSION}"
  if [ -f "$src" ]; then
    cp -f "$src" "$STAGE/$dst"
  else
    # dist/ 里已清理时从 OSS 回源：v2 发布名与 OSS 对象名一致
    oss_url="$OSS_BASE/$OSS_DIR/$dst"
    echo "  本地缺少 $src → 回源 $oss_url"
    if ! curl -fsSL --retry 2 --max-time 300 "$oss_url" -o "$STAGE/$dst"; then
      if [ "${ALLOW_MISSING_ATTACHMENTS:-false}" = "true" ]; then
        # 本次只发单平台（例如 Windows-only 修复版）：允许缺另一个平台的包
        echo "  警告：${src} 不存在且 OSS 也没有该附件，跳过该附件" >&2
        continue
      fi
      echo "错误：附件不存在 ${src}，且从 OSS 回源失败：$oss_url" >&2
      exit 1
    fi
  fi
  FILES+=("$STAGE/$dst")
  printf '  + %-44s %8s B\n' "$dst" "$(file_size "$STAGE/$dst")"
done
[ "${#FILES[@]}" -gt 0 ] || { echo "错误：没有任何可用附件，拒绝创建空 Release" >&2; exit 1; }
( cd "$STAGE" && for f in ./*; do printf '%s  %s\n' "$(sha256_hex "$f")" "${f#./}"; done > SHA256SUMS.txt )
FILES+=("$STAGE/SHA256SUMS.txt")
echo "  + SHA256SUMS.txt"

# ---- ② tag（已存在则复用）----
if git rev-parse "$TAG" >/dev/null 2>&1; then
  echo "tag $TAG 已存在，复用"
else
  echo "错误：tag $TAG 不存在。tag 由 release_tag.py 统一创建，请先执行：" >&2
  echo "  python3 tools/release/release_tag.py create --push" >&2
  exit 2
fi
if [ "$SKIP_TAG_PUSH" != "true" ] && [ "$DRY_RUN" != "true" ]; then
  echo "push tag → github"
  git push github "$TAG" 2>&1 | tail -2 || echo "  ! push github 失败（tag 可能已存在或网络问题）"
  echo "push tag → gitee(origin)"
  git push origin "$TAG" 2>&1 | tail -2 || echo "  ! push origin 失败（tag 可能已存在或网络问题）"
fi

# ---- ③ GitHub Release ----
echo ""
echo "===== GitHub：$GH_REPO"
# 标题/正文一律走 UTF-8 文件 + curl JSON，避免 Windows 控制台把中文参数转成 ????
if [ "$DRY_RUN" = "true" ]; then
  echo "  [dry-run] 创建或更新 GitHub Release + 上传 ${#FILES[@]} 个附件"
else
  if [ -z "${GH_TOKEN:-}" ]; then
    GH_TOKEN="$(gh auth token 2>/dev/null || true)"
  fi
  if [ -z "${GH_TOKEN:-}" ]; then
    echo "错误：缺少 GitHub 凭据（设 GH_TOKEN，或先 gh auth login）" >&2
    exit 3
  fi
  export GH_TOKEN
  GH_API="https://api.github.com/repos/$GH_REPO/releases"
  TAG_ENC="${TAG//+/%2B}"
  GH_REL_ID="$(curl -sS -H "Authorization: Bearer $GH_TOKEN" -H "Accept: application/vnd.github+json" \
    "$GH_API/tags/$TAG_ENC" | "$PY" -c 'import json,sys
try: print(json.load(sys.stdin).get("id",""))
except Exception: print("")' 2>/dev/null || true)"
  PAYLOAD="$(mktemp)"
  "$PY" - "$TITLE_FILE" "$NOTES" "$TAG" "$BRANCH" > "$PAYLOAD" <<'PYEOF'
import json, sys
title = open(sys.argv[1], encoding="utf-8").read()
body = open(sys.argv[2], encoding="utf-8").read()
print(json.dumps({"tag_name": sys.argv[3], "target_commitish": sys.argv[4],
                  "name": title, "body": body, "draft": False, "prerelease": False},
                 ensure_ascii=False))
PYEOF
  if [ -n "$GH_REL_ID" ]; then
    echo "Release 已存在（id=${GH_REL_ID}）→ 更新说明 + 补传附件"
    curl -fsS -X PATCH -H "Authorization: Bearer $GH_TOKEN" -H "Accept: application/vnd.github+json" \
      --data-binary @"$PAYLOAD" "$GH_API/$GH_REL_ID" >/dev/null
  else
    echo "创建 Release"
    curl -fsS -X POST -H "Authorization: Bearer $GH_TOKEN" -H "Accept: application/vnd.github+json" \
      --data-binary @"$PAYLOAD" "$GH_API" >/dev/null
  fi
  gh release upload "$TAG" "${FILES[@]}" --repo "$GH_REPO" --clobber
  rm -f "$PAYLOAD"
fi
GH_URL="https://github.com/$GH_REPO/releases/tag/$TAG"

# ---- ④ Gitee Release ----
echo ""
if [ "$SKIP_GITEE" = "true" ]; then
  echo "===== Gitee：SKIP_GITEE=true 跳过"
else
  GITEE_ENV_FILE="${GITEE_ENV_FILE:-$HOME/.config/release/gitee.env}"
  if [ -z "${GITEE_TOKEN:-}" ] && [ -f "$GITEE_ENV_FILE" ]; then
    GITEE_TOKEN="$(sed -n 's/^[[:space:]]*GITEE_TOKEN[[:space:]]*=[[:space:]]*//p' "$GITEE_ENV_FILE" | tail -1 | tr -d '\r' | sed -e 's/^"//' -e 's/"$//' -e "s/^'//" -e "s/'$//")"
  fi
  if [ -z "${GITEE_TOKEN:-}" ]; then
    if [ "${REQUIRE_GITEE:-false}" = "true" ]; then
      echo "===== Gitee：缺少 GITEE_TOKEN（REQUIRE_GITEE=true，终止）" >&2
      echo "  CI 请配置仓库 secret GITEE_TOKEN；本机可写入 $GITEE_ENV_FILE" >&2
      exit 3
    fi
    echo "===== Gitee：缺少 GITEE_TOKEN，跳过（写入 $GITEE_ENV_FILE 后重跑即可补齐）" >&2
  else
    API="https://gitee.com/api/v5/repos/$GITEE_OWNER/$GITEE_REPO"
    echo "===== Gitee：$GITEE_OWNER/$GITEE_REPO"

    REL_ID=""
    for page in 1 2 3 4 5; do
      rel_json="$(curl -sS "$API/releases?access_token=$GITEE_TOKEN&per_page=100&page=$page")"
      id="$(printf '%s' "$rel_json" | STAG="$TAG" "$PY" -c "
import json,sys,os
try: d=json.load(sys.stdin)
except Exception: d=[]
t=os.environ.get('STAG')
print(next((str(r.get('id')) for r in d if isinstance(r,dict) and r.get('tag_name')==t), ''))
" 2>/dev/null || true)"
      if [ -n "$id" ]; then REL_ID="$id"; break; fi
      n="$(printf '%s' "$rel_json" | "$PY" -c 'import json,sys
try: d=json.load(sys.stdin)
except Exception: d=[]
print(len(d) if isinstance(d,list) else 0)' 2>/dev/null || echo 0)"
      [ "${n:-0}" -lt 100 ] && break
    done

    if [ "$DRY_RUN" = "true" ]; then
      echo "  [dry-run] 创建或更新 release（tag=${TAG}）+ 上传 ${#FILES[@]} 个附件"
    elif [ -n "$REL_ID" ]; then
      echo "Release 已存在（id=${REL_ID}）→ 更新说明 + 补传附件"
      # Gitee 的 PATCH 要求 tag_name / body / name 一起传；缺字段会 400，必须检查状态码
      code="$(curl -sS -o /tmp/gitee-rel.json -w '%{http_code}' -X PATCH "$API/releases/$REL_ID" \
        --data-urlencode "access_token=$GITEE_TOKEN" \
        --data-urlencode "tag_name=$TAG" \
        --data-urlencode "name@$TITLE_ARG" \
        --data-urlencode "body@$NOTES")"
      if [ "$code" = "200" ] || [ "$code" = "201" ]; then
        echo "  ✓ 标题/说明已更新"
      else
        echo "  ✗ 更新标题/说明失败（HTTP ${code}）"; head -c 300 /tmp/gitee-rel.json; echo
      fi
      for f in "${FILES[@]}"; do
        base="$(basename "$f")"
        exists="$(curl -sS "$API/releases/$REL_ID/attach_files?access_token=$GITEE_TOKEN" \
          | BASE="$base" "$PY" -c "
import json,sys,os
try: d=json.load(sys.stdin)
except Exception: d=[]
b=os.environ.get('BASE')
print('1' if any(isinstance(a,dict) and a.get('name')==b for a in d) else '0')" 2>/dev/null || echo 0)"
        if [ "$exists" = "1" ]; then
          echo "  跳过（已存在）$base"
        else
          code="$(curl -sS -o /tmp/gitee-attach.json -w '%{http_code}' -X POST \
            "$API/releases/$REL_ID/attach_files" \
            -F "access_token=$GITEE_TOKEN" -F "file=@$f")"
          if [ "$code" = "201" ] || [ "$code" = "200" ]; then echo "  ✓ $base"; else
            echo "  ✗ ${base}（HTTP ${code}）"; head -c 300 /tmp/gitee-attach.json; echo; fi
        fi
      done
    else
      echo "创建 Release"
      resp="$(curl -sS -X POST "$API/releases" \
        --data-urlencode "access_token=$GITEE_TOKEN" \
        --data-urlencode "tag_name=$TAG" \
        --data-urlencode "name@$TITLE_ARG" \
        --data-urlencode "body@$NOTES" \
        --data-urlencode "target_commitish=$BRANCH" \
        --data-urlencode "prerelease=false")"
      REL_ID="$(printf '%s' "$resp" | "$PY" -c 'import json,sys
try: print(json.load(sys.stdin).get("id",""))
except Exception: print("")' 2>/dev/null || true)"
      if [ -z "$REL_ID" ]; then
        echo "  ✗ 创建失败：$(printf '%s' "$resp" | head -c 400)" >&2
      else
        echo "  已创建（id=${REL_ID}），上传附件"
        for f in "${FILES[@]}"; do
          base="$(basename "$f")"
          code="$(curl -sS -o /tmp/gitee-attach.json -w '%{http_code}' -X POST \
            "$API/releases/$REL_ID/attach_files" \
            -F "access_token=$GITEE_TOKEN" -F "file=@$f")"
          if [ "$code" = "201" ] || [ "$code" = "200" ]; then echo "  ✓ $base"; else
            echo "  ✗ ${base}（HTTP ${code}）"; head -c 300 /tmp/gitee-attach.json; echo; fi
        done
      fi
    fi
  fi
fi
GITEE_URL="https://gitee.com/$GITEE_OWNER/$GITEE_REPO/releases/tag/$TAG"

# ---- ⑤ 汇总 ----
echo ""
echo "================ 发行完成 ================"
echo "版本：   ${VERSION}（tag ${TAG}）"
echo "GitHub： $GH_URL"
[ "$SKIP_GITEE" = "true" ] && echo "Gitee：  （已跳过）" || echo "Gitee：  $GITEE_URL"
echo "附件："
for f in "${FILES[@]}"; do
  printf '  %-44s %s\n' "$(basename "$f")" "$(sha256_hex "$f" | cut -c1-16)"
done
