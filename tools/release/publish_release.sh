#!/usr/bin/env bash
# 发行脚本：把「已构建并已发布到 OSS 的产物」发布为 GitHub + Gitee 的 Release
#
# 用法：
#   tools/release/publish_release.sh                    # 正式发布
#   DRY_RUN=true tools/release/publish_release.sh       # 只打印将执行的动作
#   SKIP_GITEE=true tools/release/publish_release.sh    # 只发 GitHub
#   SKIP_TAG_PUSH=true tools/release/publish_release.sh # 不 push tag（tag 已在远端）
#
# 凭据：
#   GitHub  使用 gh CLI（需已 gh auth login，具备 repo 权限）
#   Gitee   环境变量 GITEE_TOKEN，或 ~/.config/release/gitee.env 中的 GITEE_TOKEN=（权限 600）
#
# 幂等：
#   - tag 已存在则复用，不覆盖
#   - Release 已存在则更新标题/说明，缺失附件补传，已存在同名附件跳过
#   - 任一平台失败不回滚另一平台，重新执行即可补齐
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

# ===== 按仓库自动识别配置（三仓共用同一份脚本）=====
PRODUCT_KEY="$(basename "$REPO_ROOT")"
case "$PRODUCT_KEY" in
  xiaotu-inventory)
    PRODUCT_NAME="小兔进销存"
    GH_REPO="LinYuwei82/xiaotu-inventory"
    GITEE_OWNER="lin-yuwei168"; GITEE_REPO="xiaotu-inventory"
    VERSION_FILE="VERSION"
    ATT_SRC=(
      "build/package/xiaotu-inventory-{V}.dmg"
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
    VERSION_FILE="VERSION.txt"
    ATT_SRC=(
      "dist/Mote-macOS-native-{V}.dmg"
      "dist/mote-{V}-win64-setup.exe"
      "dist/mote-{V}-win64-portable.zip"
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
    VERSION_FILE="VERSION.txt"
    ATT_SRC=(
      "dist/devcard-{V}-macos-arm64.dmg"
      "dist/devcard-{V}-win64-setup.exe"
      "dist/devcard-{V}-win64-portable.zip"
    )
    ATT_DST=(
      "devcard-{V}-macos-arm64.dmg"
      "devcard-{V}-win64-setup.exe"
      "devcard-{V}-win64-portable.zip"
    )
    ;;
  *)
    echo "未识别的仓库目录：$PRODUCT_KEY（脚本只支持 xiaotu-inventory / mote / dev_card_cpp）" >&2
    exit 2 ;;
esac

VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
[ -n "$VERSION" ] || { echo "无法从 $VERSION_FILE 读取版本号" >&2; exit 1; }
TAG="${TAG:-v$VERSION}"
TITLE="${TITLE:-$PRODUCT_NAME $VERSION}"
NOTES="${NOTES_FILE:-docs/release-notes-$VERSION.md}"
DRY_RUN="${DRY_RUN:-false}"
SKIP_GITEE="${SKIP_GITEE:-false}"
SKIP_TAG_PUSH="${SKIP_TAG_PUSH:-false}"
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
[ -f "$NOTES" ] || { echo "发行说明不存在：$NOTES" >&2; exit 1; }

echo "===== 发行 · $PRODUCT_NAME ${VERSION}（tag ${TAG}）====="
echo "仓库：$REPO_ROOT   分支：$BRANCH"
echo "说明：$NOTES"

# ---- ① 收集附件 + 生成 SHA256SUMS.txt ----
STAGE="$(mktemp -d -t release-stage)"
trap 'rm -rf "$STAGE"' EXIT
FILES=()
for i in "${!ATT_SRC[@]}"; do
  src="${ATT_SRC[$i]//\{V\}/$VERSION}"
  dst="${ATT_DST[$i]//\{V\}/$VERSION}"
  [ -f "$src" ] || {
    echo "错误：附件不存在 $src" >&2
    echo "  提示：Windows 产物若已从 dist/ 清掉，可从 https://dl.xiaotu.online/ 重新下载后放回该路径" >&2
    exit 1; }
  cp -f "$src" "$STAGE/$dst"
  FILES+=("$STAGE/$dst")
  printf '  + %-44s %8s B\n' "$dst" "$(stat -f %z "$src")"
done
( cd "$STAGE" && shasum -a 256 ./* > SHA256SUMS.txt )
FILES+=("$STAGE/SHA256SUMS.txt")
echo "  + SHA256SUMS.txt"

# ---- ② tag（已存在则复用）----
if git rev-parse "$TAG" >/dev/null 2>&1; then
  echo "tag $TAG 已存在，复用"
else
  echo "创建 tag $TAG"
  [ "$DRY_RUN" = "true" ] || git tag -a "$TAG" -m "$TITLE"
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
if gh release view "$TAG" --repo "$GH_REPO" >/dev/null 2>&1; then
  echo "Release 已存在 → 更新说明 + 补传附件"
  if [ "$DRY_RUN" = "true" ]; then
    echo "  [dry-run] gh release edit $TAG --notes-file $NOTES"
    echo "  [dry-run] gh release upload $TAG <${#FILES[@]} 个附件> --clobber"
  else
    gh release edit "$TAG" --repo "$GH_REPO" --title "$TITLE" --notes-file "$NOTES"
    gh release upload "$TAG" "${FILES[@]}" --repo "$GH_REPO" --clobber
  fi
else
  echo "创建 Release"
  if [ "$DRY_RUN" = "true" ]; then
    echo "  [dry-run] gh release create $TAG --title $TITLE --notes-file $NOTES <${#FILES[@]} 个附件>"
  else
    gh release create "$TAG" --repo "$GH_REPO" --title "$TITLE" \
      --notes-file "$NOTES" --verify-tag "${FILES[@]}"
  fi
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
    echo "===== Gitee：缺少 GITEE_TOKEN，跳过（写入 $GITEE_ENV_FILE 后重跑即可补齐）" >&2
  else
    API="https://gitee.com/api/v5/repos/$GITEE_OWNER/$GITEE_REPO"
    echo "===== Gitee：$GITEE_OWNER/$GITEE_REPO"

    REL_ID=""
    for page in 1 2 3 4 5; do
      rel_json="$(curl -sS "$API/releases?access_token=$GITEE_TOKEN&per_page=100&page=$page")"
      id="$(printf '%s' "$rel_json" | STAG="$TAG" python3 -c "
import json,sys,os
try: d=json.load(sys.stdin)
except Exception: d=[]
t=os.environ.get('STAG')
print(next((str(r.get('id')) for r in d if isinstance(r,dict) and r.get('tag_name')==t), ''))
" 2>/dev/null || true)"
      if [ -n "$id" ]; then REL_ID="$id"; break; fi
      n="$(printf '%s' "$rel_json" | python3 -c 'import json,sys
try: d=json.load(sys.stdin)
except Exception: d=[]
print(len(d) if isinstance(d,list) else 0)' 2>/dev/null || echo 0)"
      [ "${n:-0}" -lt 100 ] && break
    done

    if [ "$DRY_RUN" = "true" ]; then
      echo "  [dry-run] 创建或更新 release（tag=${TAG}）+ 上传 ${#FILES[@]} 个附件"
    elif [ -n "$REL_ID" ]; then
      echo "Release 已存在（id=$REL_ID）→ 更新说明 + 补传附件"
      curl -sS -X PATCH "$API/releases/$REL_ID" \
        --data-urlencode "access_token=$GITEE_TOKEN" \
        --data-urlencode "name=$TITLE" \
        --data-urlencode "body@$NOTES" >/dev/null
      for f in "${FILES[@]}"; do
        base="$(basename "$f")"
        exists="$(curl -sS "$API/releases/$REL_ID/attach_files?access_token=$GITEE_TOKEN" \
          | BASE="$base" python3 -c "
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
            echo "  ✗ $base（HTTP $code）"; head -c 300 /tmp/gitee-attach.json; echo; fi
        fi
      done
    else
      echo "创建 Release"
      resp="$(curl -sS -X POST "$API/releases" \
        --data-urlencode "access_token=$GITEE_TOKEN" \
        --data-urlencode "tag_name=$TAG" \
        --data-urlencode "name=$TITLE" \
        --data-urlencode "body@$NOTES" \
        --data-urlencode "target_commitish=$BRANCH" \
        --data-urlencode "prerelease=false")"
      REL_ID="$(printf '%s' "$resp" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("id",""))
except Exception: print("")' 2>/dev/null || true)"
      if [ -z "$REL_ID" ]; then
        echo "  ✗ 创建失败：$(printf '%s' "$resp" | head -c 400)" >&2
      else
        echo "  已创建（id=$REL_ID），上传附件"
        for f in "${FILES[@]}"; do
          base="$(basename "$f")"
          code="$(curl -sS -o /tmp/gitee-attach.json -w '%{http_code}' -X POST \
            "$API/releases/$REL_ID/attach_files" \
            -F "access_token=$GITEE_TOKEN" -F "file=@$f")"
          if [ "$code" = "201" ] || [ "$code" = "200" ]; then echo "  ✓ $base"; else
            echo "  ✗ $base（HTTP $code）"; head -c 300 /tmp/gitee-attach.json; echo; fi
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
  printf '  %-44s %s\n' "$(basename "$f")" "$(shasum -a 256 "$f" | cut -c1-16)"
done
