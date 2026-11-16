#!/usr/bin/env bash
# 小兔进销存 · macOS OSS 发布（自动更新链路）
#
# 输入 dmg → 生成清单与 appcast → 上传 OSS → purge CDN。
# 与 make_dmg.sh 配合：make_dmg.sh 产出 build/package/xiaotu-inventory-<ver>.dmg，
# 本脚本负责把它推到 OSS 上并让客户端自动更新能拉到。
#
# 依赖：
#   - ossutil（阿里云官方 CLI）：https://help.aliyun.com/document_detail/120075.html
#     需预先 ossutil config 配好 AccessKey、bucket、region、endpoint
#   - python3 + cryptography（tools/package/.venv）
#   - openssl、shasum、PlistBuddy（macOS 自带）
#
# 用法：
#   tools/package/publish_oss.sh              # 完整发布（需要 ossutil）
#   OSS_SKIP=true tools/package/publish_oss.sh # dry run：只本地生成 + 验签
#
# 环境变量：
#   DMG_PATH              dmg 文件路径（默认 build/package/xiaotu-inventory-<version>.dmg）
#   OSS_BUCKET            OSS bucket 名（OSS_SKIP=true 时可省）
#   OSS_REGION            OSS 区域（OSS_SKIP=true 时可省）
#   CDN_DOMAIN            对外访问域名（https://...），清单里的 dmg url 用这个
#   CHANNEL               stable / beta（默认 stable）
#   SIGNING_KEY           Ed25519 私钥 PEM（默认 ~/.config/xiaotu-updates/update_signing_2026.pem）
#   KEY_ID                key_id 元数据（默认 2026-11）
#   SCHEMA_MIN / SCHEMA_MAX    schema 兼容范围（默认 1 / 39）
#   API_MIN / API_MAX          API version 范围（默认 1 / 1）
#   MANDATORY             "true" 强制升级（默认 false）
#   RELEASE_NOTES_URL     发布说明链接（默认空）
#   PURGE                 "false" 跳过 purge（默认 true）
#   OSS_SKIP              "true" 只跑本地生成 + 验签，不碰 OSS（dry run）

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

VERSION="$(tr -d '[:space:]' < "$REPO_ROOT/VERSION" 2>/dev/null)"
if [ -z "$VERSION" ]; then
  echo "无法从仓库根 VERSION 文件读取版本号。" >&2
  exit 1
fi

usage() {
  sed -n '2,/^EOF$/p' "$0" >/dev/null || true
  # 简单输出：打印注释块
  sed -n '1,/^EOF$/p' "$0" | sed '1d;/^#/!d;s/^# \{0,1\}//' | head -40
}

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  "") ;;
  *) echo "未知参数：$1（用 --help 查看用法）" >&2; exit 2 ;;
esac

# ---- 参数 ----
OSS_BUCKET="${OSS_BUCKET:-}"
OSS_REGION="${OSS_REGION:-}"
CDN_DOMAIN="${CDN_DOMAIN:-}"
OSS_SKIP="${OSS_SKIP:-false}"

if [ "$OSS_SKIP" != "true" ]; then
  if [ -z "$OSS_BUCKET" ] || [ -z "$OSS_REGION" ] || [ -z "$CDN_DOMAIN" ]; then
    echo "缺少必填：OSS_BUCKET / OSS_REGION / CDN_DOMAIN" >&2
    echo "如需只跑本地生成验证：OSS_SKIP=true tools/package/publish_oss.sh" >&2
    exit 1
  fi
  case "$CDN_DOMAIN" in
    https://*|http://*) ;;
    *) echo "CDN_DOMAIN 必须以 http(s):// 开头" >&2; exit 1 ;;
  esac
else
  echo "[dry-run] OSS_SKIP=true：跳过 OSS 上传与 purge"
  CDN_DOMAIN="${CDN_DOMAIN:-https://example.com}"
fi

CHANNEL="${CHANNEL:-stable}"
case "$CHANNEL" in stable|beta) ;; *) echo "CHANNEL 必须是 stable/beta" >&2; exit 1 ;; esac

DEFAULT_DMG="$REPO_ROOT/build/package/xiaotu-inventory-$VERSION.dmg"
DMG_PATH="${DMG_PATH:-$DEFAULT_DMG}"
if [ ! -f "$DMG_PATH" ]; then
  echo "dmg 不存在：$DMG_PATH" >&2
  exit 1
fi

SIGNING_KEY="${SIGNING_KEY:-$HOME/.config/xiaotu-updates/update_signing_2026.pem}"
if [ ! -f "$SIGNING_KEY" ]; then
  echo "签名私钥不存在：$SIGNING_KEY" >&2
  echo "用 openssl genpkey -algorithm ED25519 -out $SIGNING_KEY 生成" >&2
  exit 1
fi
SIGNING_PUB="${SIGNING_KEY%.pem}.pub.pem"
PUB_BASE64="$(openssl pkey -pubin -in "$SIGNING_PUB" -outform DER 2>/dev/null \
  | tail -c 32 | base64 | tr -d '\n')"

KEY_ID="${KEY_ID:-2026-11}"
SCHEMA_MIN="${SCHEMA_MIN:-1}"
SCHEMA_MAX="${SCHEMA_MAX:-39}"
API_MIN="${API_MIN:-1}"
API_MAX="${API_MAX:-1}"
MANDATORY="${MANDATORY:-}"
RELEASE_NOTES_URL="${RELEASE_NOTES_URL:-}"
PURGE="${PURGE:-true}"

# ---- 依赖 ----
VENV_PY="$REPO_ROOT/tools/package/.venv/bin/python"
if [ ! -x "$VENV_PY" ]; then
  python3 -m venv "$REPO_ROOT/tools/package/.venv"
  "$REPO_ROOT/tools/package/.venv/bin/pip" install -q -r "$REPO_ROOT/tools/package/requirements.txt"
fi
command -v openssl >/dev/null 2>&1 || { echo "未找到 openssl" >&2; exit 1; }
command -v shasum >/dev/null 2>&1 || { echo "未找到 shasum" >&2; exit 1; }

if [ "$OSS_SKIP" != "true" ]; then
  command -v ossutil >/dev/null 2>&1 || {
    echo "未找到 ossutil：brew install ossutil 或 https://github.com/aliyun/ossutil/releases" >&2
    exit 1; }
  ossutil ls "oss://$OSS_BUCKET" >/dev/null 2>&1 || {
    echo "ossutil 无法访问 oss://$OSS_BUCKET，请先 ossutil config" >&2
    exit 1; }
fi

# ---- 元数据 ----
# 从 dmg 内部 .app 的 Info.plist 读构建号（最可靠），不行才从文件名推断
DMG_MOUNT="$(mktemp -d -t xiaotu-dmg)"
hdiutil attach "$DMG_PATH" -mountpoint "$DMG_MOUNT" -nobrowse -readonly >/dev/null 2>&1 || true
APP_IN_DMG="$DMG_MOUNT/小兔进销存.app"
if [ ! -d "$APP_IN_DMG" ]; then
  APP_IN_DMG="$(find "$DMG_MOUNT" -name "*.app" -maxdepth 2 2>/dev/null | head -1 || true)"
fi

BUILD=""
if [ -n "$APP_IN_DMG" ] && [ -f "$APP_IN_DMG/Contents/Info.plist" ]; then
  PLIST_BUILD="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' \
    "$APP_IN_DMG/Contents/Info.plist" 2>/dev/null || true)"
  PLIST_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
    "$APP_IN_DMG/Contents/Info.plist" 2>/dev/null || true)"
  if [[ "$PLIST_BUILD" =~ ^[0-9]+$ ]]; then
    BUILD="$PLIST_BUILD"
  fi
  if [ "$PLIST_VERSION" != "$VERSION" ]; then
    echo "警告：dmg 内版本 $PLIST_VERSION 与仓库 VERSION $VERSION 不一致" >&2
  fi
fi
# 清理挂载
hdiutil detach "$DMG_MOUNT" >/dev/null 2>&1 || true
rm -rf "$DMG_MOUNT"

# 退到：从文件名推断 build 号
if [ -z "$BUILD" ] || ! [[ "$BUILD" =~ ^[0-9]+$ ]]; then
  DMG_BASENAME="$(basename "$DMG_PATH")"
  BUILD="$(echo "$DMG_BASENAME" | sed -E 's/.*-([0-9]{6,})\.dmg$/\1/')"
  if ! [[ "$BUILD" =~ ^[0-9]+$ ]]; then
    echo "无法从 dmg 推断构建号（文件名：$DMG_BASENAME）" >&2
    exit 1
  fi
fi

DMG_SIZE="$(stat -f %z "$DMG_PATH")"
DMG_SHA256="$(shasum -a 256 "$DMG_PATH" | awk '{print $1}')"
PUBLISHED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
DMG_BASENAME="$(basename "$DMG_PATH")"

echo "===== 小兔进销存 OSS 发布 ====="
echo "版本：$VERSION  构建号：$BUILD  channel：$CHANNEL"
echo "dmg：$DMG_PATH"
echo "     $DMG_SIZE 字节  sha256=$DMG_SHA256"
echo "签名：key_id=$KEY_ID  pubkey=$PUB_BASE64"
echo "域名：$CDN_DOMAIN"
[ "$OSS_SKIP" = "true" ] && echo "(dry-run：不上传)"

STAGE="$(mktemp -d -t xiaotu-oss-stage)"
trap 'rm -rf "$STAGE"' EXIT

# ---- ① 上传 dmg ----
DMG_OSS="oss://$OSS_BUCKET/releases/macos/$VERSION/$DMG_BASENAME"
DMG_PUBLIC_URL="$CDN_DOMAIN/releases/macos/$VERSION/$DMG_BASENAME"

if [ "$OSS_SKIP" != "true" ]; then
  echo ""
  echo "===== ① 上传 dmg → $DMG_OSS"
  ossutil cp "$DMG_PATH" "$DMG_OSS" \
    --meta "Cache-Control:public, max-age=31536000, immutable" \
    --meta "Content-Type:application/octet-stream" \
    -f
  echo "  上传完成"
else
  echo ""
  echo "===== ① [dry-run] 将上传 $DMG_OSS"
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

# ---- ③ 本地验签 ----
echo ""
echo "===== ③ 本地验签"
"$VENV_PY" "$REPO_ROOT/tools/package/update_manifest.py" verify \
  --manifest "$MANIFEST_LOCAL" --pub "$SIGNING_PUB" --file "$DMG_PATH"

# 打印关键字段便于确认
MANIFEST_KEY_ID="$($VENV_PY -c "import json;print(json.load(open('$MANIFEST_LOCAL'))['key_id'])")"
MANIFEST_ROLLOUT="$($VENV_PY -c "import json;print(json.load(open('$MANIFEST_LOCAL'))['rollout_percent'])")"
MANIFEST_MANDATORY="$($VENV_PY -c "import json;print(json.load(open('$MANIFEST_LOCAL')).get('mandatory', False))")"
echo "  key_id=$MANIFEST_KEY_ID  rollout=${MANIFEST_ROLLOUT}%  mandatory=$MANIFEST_MANDATORY"

# ---- ④ 上传 latest.json + appcast.xml ----
LATEST_OSS="oss://$OSS_BUCKET/updates/macos/$CHANNEL/latest.json"
APPCAST_OSS="oss://$OSS_BUCKET/updates/macos/$CHANNEL/appcast.xml"

if [ "$OSS_SKIP" != "true" ]; then
  echo ""
  echo "===== ④ 上传 latest.json + appcast.xml"
  ossutil cp "$MANIFEST_LOCAL" "$LATEST_OSS" \
    --meta "Cache-Control:no-cache, max-age=300" \
    --meta "Content-Type:application/json" -f
  ossutil cp "$APPCAST_LOCAL" "$APPCAST_OSS" \
    --meta "Cache-Control:no-cache, max-age=300" \
    --meta "Content-Type:application/rss+xml" -f
  echo "  latest.json → $LATEST_OSS"
  echo "  appcast.xml → $APPCAST_OSS"
else
  echo ""
  echo "===== ④ [dry-run] 将上传 $LATEST_OSS + $APPCAST_OSS"
  echo "  latest.json 内容预览："
  head -c 400 "$MANIFEST_LOCAL"
  echo "... (截断)"
fi

# ---- ⑤ purge CDN ----
if [ "$OSS_SKIP" != "true" ]; then
  if [ "$PURGE" = "true" ]; then
    echo ""
    echo "===== ⑤ purge CDN 缓存"
    for url in "$DMG_PUBLIC_URL" \
               "$CDN_DOMAIN/updates/macos/$CHANNEL/latest.json" \
               "$CDN_DOMAIN/updates/macos/$CHANNEL/appcast.xml"; do
      if command -v aliyun >/dev/null 2>&1; then
        aliyun cdn RefreshObjectCaches --ObjectPath "$url" >/dev/null 2>&1 && \
          echo "  已 purge: $url" || echo "  purge 失败：$url（手动 purge）"
      else
        echo "  (手动 purge：$url)"
      fi
    done
    echo "  CDN purge 完成，5 分钟内全网生效"
  else
    echo ""
    echo "===== ⑤ 跳过 purge（PURGE=false）"
    echo "  提醒：CDN 未 purge，新清单最多 5 分钟后自动生效"
  fi
fi

# ---- ⑥ 完成 ----
MANIFEST_PUBLIC="$CDN_DOMAIN/updates/macos/$CHANNEL/latest.json"
APPCAST_PUBLIC="$CDN_DOMAIN/updates/macos/$CHANNEL/appcast.xml"

echo ""
echo "================ 发布完成 ================"
echo "版本 / build / channel：  $VERSION / $BUILD / $CHANNEL"
echo "dmg 下载地址：           $DMG_PUBLIC_URL"
echo "  sha256 / size：        $DMG_SHA256 / $DMG_SIZE 字节"
echo "latest.json：            $MANIFEST_PUBLIC"
echo "appcast.xml：            $APPCAST_PUBLIC"
echo "schema 范围：            $SCHEMA_MIN ~ $SCHEMA_MAX  api：$API_MIN ~ $API_MAX"
echo "key_id：                 $MANIFEST_KEY_ID  rollout：${MANIFEST_ROLLOUT}%"
echo ""
echo "→ 客户端打包注入（make_dmg.sh）："
echo "  XIAOTU_UPDATE_MANIFEST_URL=$MANIFEST_PUBLIC"
echo "  XIAOTU_UPDATE_FEED_URL=$APPCAST_PUBLIC"
echo "  XIAOTU_UPDATE_PUBKEY=$PUB_BASE64"
echo ""
echo "→ 立即验证："
if [ "$OSS_SKIP" = "true" ]; then
  echo "  (dry-run：用本地文件)"
  echo "  python3 tools/package/update_manifest.py verify --manifest $MANIFEST_LOCAL --pub $SIGNING_PUB --file $DMG_PATH"
else
  echo "  curl -s $MANIFEST_PUBLIC | python3 -m json.tool | head -15"
  echo "  python3 tools/package/update_manifest.py verify --manifest <(curl -s $MANIFEST_PUBLIC) --pub $SIGNING_PUB --file $DMG_PATH"
fi
