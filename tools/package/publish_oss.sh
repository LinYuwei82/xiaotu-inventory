#!/usr/bin/env bash
# 小兔在线 · OSS 发布脚本（自动更新链路）
#
# 输入 dmg → 生成 Ed25519 签名的 latest.json + appcast.xml → 上传 dl.xiaotu.online → purge CDN。
# 三款产品共用同一份脚本，通过 PRODUCT 选择目标子目录：
#
#   PRODUCT=xiaotu  → dl.xiaotu.online/xiaotu/
#   PRODUCT=devcard → dl.xiaotu.online/devcard/
#   PRODUCT=mote    → dl.xiaotu.online/mote/
#
# 目录结构（与 index.html 已在用的保持一致）：
#   oss://<bucket>/<product>/<app>-<ver>.dmg                ← 长缓存 / immutable
#   oss://<bucket>/<product>/updates/<channel>/latest.json  ← 5 分钟缓存
#   oss://<bucket>/<product>/updates/<channel>/appcast.xml  ← 5 分钟缓存
#
# 依赖：ossutil、python3 + cryptography（tools/package/.venv）、openssl、shasum、PlistBuddy
#
# 用法：
#   # 完整发布（需要 ossutil + 预配置 bucket）
#   OSS_BUCKET=xxx OSS_REGION=cn-hangzhou PRODUCT=xiaotu tools/package/publish_oss.sh
#
#   # dry-run：只本地生成 + 验签，不碰 OSS
#   OSS_SKIP=true PRODUCT=xiaotu tools/package/publish_oss.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

VERSION="$(tr -d '[:space:]' < "$REPO_ROOT/VERSION" 2>/dev/null)"
[ -n "$VERSION" ] || { echo "无法从仓库根 VERSION 文件读取版本号。" >&2; exit 1; }

usage() {
  sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
}
case "${1:-}" in -h|--help) usage; exit 0 ;; "") ;;
  *) echo "未知参数：$1（用 --help 查看用法）" >&2; exit 2 ;; esac

# ---- 核心参数 ----
PRODUCT="${PRODUCT:-xiaotu}"
case "$PRODUCT" in
  xiaotu|devcard|mote) ;;
  *) echo "PRODUCT 必须是 xiaotu / devcard / mote" >&2; exit 1 ;;
esac

OSS_BUCKET="${OSS_BUCKET:-}"
OSS_REGION="${OSS_REGION:-}"
CDN_DOMAIN="${CDN_DOMAIN:-https://dl.xiaotu.online}"
OSS_SKIP="${OSS_SKIP:-false}"

if [ "$OSS_SKIP" != "true" ]; then
  if [ -z "$OSS_BUCKET" ] || [ -z "$OSS_REGION" ]; then
    echo "缺少必填：OSS_BUCKET / OSS_REGION" >&2
    echo "需要 OSS_ACCESS_KEY_ID / OSS_ACCESS_KEY_SECRET 环境变量" >&2
    echo "如需只跑本地生成验证：OSS_SKIP=true tools/package/publish_oss.sh" >&2
    exit 1
  fi
  case "$CDN_DOMAIN" in https://*|http://*) ;;
    *) echo "CDN_DOMAIN 必须以 http(s):// 开头" >&2; exit 1 ;; esac
else
  echo "[dry-run] OSS_SKIP=true：跳过 OSS 上传与 purge"
fi

# ---- 可选参数 ----
CHANNEL="${CHANNEL:-stable}"
case "$CHANNEL" in stable|beta) ;; *) echo "CHANNEL 必须是 stable/beta" >&2; exit 1 ;; esac

DEFAULT_DMG="$REPO_ROOT/build/package/xiaotu-inventory-$VERSION.dmg"
DMG_PATH="${DMG_PATH:-$DEFAULT_DMG}"
[ -f "$DMG_PATH" ] || { echo "dmg 不存在：$DMG_PATH" >&2; exit 1; }

DEFAULT_KEY="$HOME/.config/xiaotu-updates/update_signing_2026.pem"
SIGNING_KEY="${SIGNING_KEY:-$DEFAULT_KEY}"
[ -f "$SIGNING_KEY" ] || {
  echo "签名私钥不存在：$SIGNING_KEY" >&2
  echo "用 openssl genpkey -algorithm ED25519 -out $SIGNING_KEY 生成" >&2; exit 1; }
SIGNING_PUB="${SIGNING_KEY%.pem}.pub.pem"

KEY_ID="${KEY_ID:-2026-11}"
SCHEMA_MIN="${SCHEMA_MIN:-1}"
SCHEMA_MAX="${SCHEMA_MAX:-39}"
API_MIN="${API_MIN:-1}"
API_MAX="${API_MAX:-1}"
MANDATORY="${MANDATORY:-}"
RELEASE_NOTES_URL="${RELEASE_NOTES_URL:-}"
PURGE="${PURGE:-true}"

# ---- 依赖检查 ----
VENV_PY="$REPO_ROOT/tools/package/.venv/bin/python"
if [ ! -x "$VENV_PY" ]; then
  python3 -m venv "$REPO_ROOT/tools/package/.venv"
  "$REPO_ROOT/tools/package/.venv/bin/pip" install -q -r "$REPO_ROOT/tools/package/requirements.txt"
fi
for dep in openssl shasum; do
  command -v "$dep" >/dev/null 2>&1 || { echo "未找到 $dep" >&2; exit 1; }
done
if [ "$OSS_SKIP" != "true" ]; then
  # 用 Python oss2 SDK 上传（无需 ossutil 二进制）
  OSS_PY="${OSS_PY:-$HOME/.venv-oss/bin/python}"
  if [ ! -x "$OSS_PY" ]; then
    OSS_PY="$REPO_ROOT/tools/package/.venv/bin/python"
  fi
  if ! "$OSS_PY" -c "import oss2" 2>/dev/null; then
    echo "未找到 oss2 Python SDK，正在安装..."
    python3 -m venv "$HOME/.venv-oss"
    "$HOME/.venv-oss/bin/pip" install -q oss2
    OSS_PY="$HOME/.venv-oss/bin/python"
  fi
  if [ -z "${OSS_ACCESS_KEY_ID:-}" ] || [ -z "${OSS_ACCESS_KEY_SECRET:-}" ]; then
    echo "缺少 OSS_ACCESS_KEY_ID / OSS_ACCESS_KEY_SECRET 环境变量" >&2; exit 1
  fi
fi

# ---- 元数据：从 dmg 的 Info.plist 读构建号 ----
DMG_MOUNT="$(mktemp -d -t xiaotu-dmg)"
hdiutil attach "$DMG_PATH" -mountpoint "$DMG_MOUNT" -nobrowse -readonly >/dev/null 2>&1 || true
APP_IN_DMG="$(find "$DMG_MOUNT" -name "*.app" -maxdepth 2 2>/dev/null | head -1 || true)"
BUILD=""
PLIST_VERSION=""
if [ -n "$APP_IN_DMG" ] && [ -f "$APP_IN_DMG/Contents/Info.plist" ]; then
  BUILD="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' \
    "$APP_IN_DMG/Contents/Info.plist" 2>/dev/null || true)"
  PLIST_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
    "$APP_IN_DMG/Contents/Info.plist" 2>/dev/null || true)"
  if [ "$PLIST_VERSION" != "$VERSION" ]; then
    echo "警告：dmg 内版本 $PLIST_VERSION ≠ 仓库 VERSION $VERSION" >&2
  fi
fi
hdiutil detach "$DMG_MOUNT" >/dev/null 2>&1 || true
rm -rf "$DMG_MOUNT"

# 从文件名推断 build 号
if ! [[ "$BUILD" =~ ^[0-9]{4,}$ ]]; then
  DMG_BASENAME_FN="$(basename "$DMG_PATH")"
  BUILD="$(echo "$DMG_BASENAME_FN" | sed -E 's/.*-([0-9]{6,})\.dmg$/\1/')"
  [[ "$BUILD" =~ ^[0-9]{4,}$ ]] || { echo "无法从 dmg 推断构建号（$DMG_BASENAME_FN）" >&2; exit 1; }
fi

DMG_SIZE="$(stat -f %z "$DMG_PATH")"
DMG_SHA256="$(shasum -a 256 "$DMG_PATH" | awk '{print $1}')"
PUBLISHED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
DMG_BASENAME="$(basename "$DMG_PATH")"
PUB_BASE64="$(openssl pkey -pubin -in "$SIGNING_PUB" -outform DER 2>/dev/null \
  | tail -c 32 | base64 | tr -d '\n')"

# ---- 路径（纯 object key，不含 bucket/endpoint）----
PRODUCT_DIR="$PRODUCT"
DMG_KEY="$PRODUCT_DIR/$DMG_BASENAME"
DMG_PUBLIC_URL="$CDN_DOMAIN/$DMG_KEY"
LATEST_KEY="$PRODUCT_DIR/updates/$CHANNEL/latest.json"
APPCAST_KEY="$PRODUCT_DIR/updates/$CHANNEL/appcast.xml"
MANIFEST_PUBLIC="$CDN_DOMAIN/$LATEST_KEY"
APPCAST_PUBLIC="$CDN_DOMAIN/$APPCAST_KEY"

echo "===== OSS 发布 · ${PRODUCT} ====="
echo "版本 / build / channel：  ${VERSION} / ${BUILD} / ${CHANNEL}"
echo "dmg：                     ${DMG_PATH} （${DMG_SIZE} 字节）"
echo "sha256：                  ${DMG_SHA256}"
echo "公钥 base64：             ${PUB_BASE64}"
echo "bucket / region：         ${OSS_BUCKET:-<dry-run>} / ${OSS_REGION:-<dry-run>}"
echo "域名：                    ${CDN_DOMAIN}"
echo "OSS key："
echo "  dmg：                   ${DMG_KEY}"
echo "  latest.json：           ${LATEST_KEY}"
echo "  appcast.xml：           ${APPCAST_KEY}"
[ "${OSS_SKIP}" = "true" ] && echo "(dry-run：不上传)"

STAGE="$(mktemp -d -t xiaotu-oss-stage)"
trap 'rm -rf "$STAGE"' EXIT

# ---- Python OSS 上传函数（oss2 SDK）----
oss_upload() {
  # oss_upload <local_path> <oss_key> <cache_control> <content_type>
  local LP="$1" OK="$2" CC="$3" CT="$4"
  "$OSS_PY" - "$OSS_BUCKET" "$OSS_REGION" "$OSS_ACCESS_KEY_ID" "$OSS_ACCESS_KEY_SECRET" \
    "$LP" "$OK" "$CC" "$CT" <<'PY_EOF'
import sys, oss2
bucket, region, ak, sk, lp, ok, cc, ct = sys.argv[1:]
endpoint = f"https://oss-{region}.aliyuncs.com"
auth = oss2.Auth(ak, sk)
bkt = oss2.Bucket(auth, endpoint, bucket)
headers = {"Cache-Control": cc, "Content-Type": ct}
with open(lp, "rb") as f:
    bkt.put_object(ok, f, headers=headers)
print(f"  ✓ {ok}  ({ct}, {cc})")
PY_EOF
}

# ---- ① 上传 dmg（immutable，长缓存）----
if [ "$OSS_SKIP" != "true" ]; then
  echo ""
  echo "===== ① 上传 dmg → $DMG_KEY"
  oss_upload "$DMG_PATH" "$DMG_KEY" \
    "public, max-age=31536000, immutable" "application/octet-stream"
else
  echo ""
  echo "===== ① [dry-run] 将上传 $DMG_KEY"
fi

# ---- ② 生成 latest.json + appcast.xml ----
MANIFEST_LOCAL="$STAGE/latest.json"
APPCAST_LOCAL="$STAGE/appcast.xml"

MANIFEST_ARGS=(
  generate
  --file "$DMG_PATH"
  --version "$VERSION"
  --build "$BUILD"
  --url "$DMG_PUBLIC_URL"
  --platform macos
  --channel "$CHANNEL"
  --schema-min "$SCHEMA_MIN"
  --schema-max "$SCHEMA_MAX"
  --api-min "$API_MIN"
  --api-max "$API_MAX"
  --rollout 100
  --key-id "$KEY_ID"
  --key "$SIGNING_KEY"
  --out "$MANIFEST_LOCAL"
  --appcast "$APPCAST_LOCAL"
  --published-at "$PUBLISHED_AT"
)
[ -n "$MANDATORY" ] && [ "$MANDATORY" = "true" ] && \
  MANIFEST_ARGS+=(--mandatory --reason MIN_SUPPORTED --deadline "$PUBLISHED_AT")
[ -n "$RELEASE_NOTES_URL" ] && MANIFEST_ARGS+=(--release-notes-url "$RELEASE_NOTES_URL")

echo ""
echo "===== ② 生成 latest.json + appcast.xml"
"$VENV_PY" "$REPO_ROOT/tools/package/update_manifest.py" "${MANIFEST_ARGS[@]}"

# ---- ③ 本地验签（上传前的双重保险）----
echo ""
echo "===== ③ 本地验签"
"$VENV_PY" "$REPO_ROOT/tools/package/update_manifest.py" verify \
  --manifest "$MANIFEST_LOCAL" --pub "$SIGNING_PUB" --file "$DMG_PATH"

MANIFEST_KEY_ID="$($VENV_PY -c "import json;print(json.load(open('$MANIFEST_LOCAL'))['key_id'])")"
MANIFEST_ROLLOUT="$($VENV_PY -c "import json;print(json.load(open('$MANIFEST_LOCAL'))['rollout_percent'])")"
echo "  key_id=$MANIFEST_KEY_ID  rollout=${MANIFEST_ROLLOUT}%"

# ---- ④ 上传 latest.json + appcast.xml（短缓存 5 分钟）----
if [ "$OSS_SKIP" != "true" ]; then
  echo ""
  echo "===== ④ 上传 latest.json + appcast.xml"
  oss_upload "$MANIFEST_LOCAL" "$LATEST_KEY" \
    "no-cache, max-age=60" "application/json"
  oss_upload "$APPCAST_LOCAL" "$APPCAST_KEY" \
    "no-cache, max-age=60" "application/rss+xml"
else
  echo ""
  echo "===== ④ [dry-run] 将上传 $LATEST_KEY + $APPCAST_KEY"
fi

# ---- ⑤ purge CDN ----
if [ "$OSS_SKIP" != "true" ]; then
  if [ "$PURGE" = "true" ]; then
    echo ""
    echo "===== ⑤ purge CDN 缓存"
    for url in "$DMG_PUBLIC_URL" "$MANIFEST_PUBLIC" "$APPCAST_PUBLIC"; do
      if command -v aliyun >/dev/null 2>&1; then
        if aliyun cdn RefreshObjectCaches --ObjectPath "$url" >/dev/null 2>&1; then
          echo "  ✓ $url"
        else
          echo "  ✗ purge 失败：$url（手动 purge）"
        fi
      else
        echo "  (手动 purge：$url)"
      fi
    done
    echo "  CDN purge 完成，1 分钟内全网生效"
  else
    echo ""
    echo "===== ⑤ 跳过 purge（PURGE=false）"
    echo "  提醒：新 manifest 最多 1 分钟后自动生效"
  fi
fi

# ---- ⑥ 完成 ----
echo ""
echo "================ 发布完成 ================"
echo "版本 / build / channel：  $VERSION / $BUILD / $CHANNEL"
echo "dmg 下载地址：           $DMG_PUBLIC_URL"
echo "  sha256 / size：        $DMG_SHA256 / $DMG_SIZE 字节"
echo "latest.json：            $MANIFEST_PUBLIC"
echo "appcast.xml：            $APPCAST_PUBLIC"
echo "schema / api：           $SCHEMA_MIN ~ $SCHEMA_MAX / $API_MIN ~ $API_MAX"
echo "key_id / rollout：       $MANIFEST_KEY_ID / ${MANIFEST_ROLLOUT}%"
echo ""
echo "→ 客户端 Info.plist 注入（make_dmg.sh 参数）："
echo "  XIAOTU_UPDATE_MANIFEST_URL=$MANIFEST_PUBLIC"
echo "  XIAOTU_UPDATE_FEED_URL=$APPCAST_PUBLIC"
echo "  XIAOTU_UPDATE_PUBKEY=$PUB_BASE64"
echo ""
echo "→ 立即验证："
if [ "$OSS_SKIP" = "true" ]; then
  echo "  python3 tools/package/update_manifest.py verify --manifest $MANIFEST_LOCAL --pub $SIGNING_PUB --file $DMG_PATH"
else
  echo "  curl -s $MANIFEST_PUBLIC | python3 -m json.tool | head -15"
  echo "  python3 tools/package/update_manifest.py verify --manifest <(curl -s $MANIFEST_PUBLIC) --pub $SIGNING_PUB --file $DMG_PATH"
fi
