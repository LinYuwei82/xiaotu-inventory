#!/usr/bin/env bash
# 小兔进销存 · macOS 可分发包（.dmg）
#
# 一条命令从构建到出 dmg：
#   tools/package/make_dmg.sh
#
# 产物：build/package/xiaotu-inventory-<版本>.dmg
#   ├── 小兔进销存.app
#   └── 应用程序 -> /Applications
#
# 签名：
#   - 默认 ad-hoc 签名（codesign -s -），不满足 Gatekeeper 与公证要求；
#   - 设置 XIAOTU_SIGN_IDENTITY 后改用 Developer ID Application 证书签名；
#   - 设置 XIAOTU_NOTARY_PROFILE 后调用 notarytool 提交 dmg 并 staple；
#     未提供时明确打印「跳过公证」，不会静默当作已公证。
#   未签名/未公证的包首次打开会被 Gatekeeper 拦截，处理方式见 tools/package/README.md。
#
# 环境变量：
#   XIAOTU_SIGN_IDENTITY  签名证书名称（默认空，使用 ad-hoc）
#   XIAOTU_NOTARY_PROFILE notarytool keychain profile（默认空，跳过公证）
#   XIAOTU_DIST_DIR       产物目录（默认 <repo>/build/package）
#   XIAOTU_BUILD          构建号（纯数字，单调递增）；必填，缺失即失败
#   XIAOTU_UPDATE_MANIFEST_URL  单机版清单地址（https）
#   XIAOTU_UPDATE_FEED_URL      Sparkle appcast 地址（https）
#   XIAOTU_UPDATE_PUBKEY        Sparkle 公钥（Ed25519，base64，32 字节）
#   更新三项必须同时提供或同时留空；留空表示更新功能关闭。

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

APP_DISPLAY_NAME="小兔进销存"
VOLUME_NAME="小兔进销存"
BUILD_DIR="$REPO_ROOT/build/release"
BUILT_APP="$BUILD_DIR/shell/xiaotu-inventory.app"
OUT_DIR="${XIAOTU_DIST_DIR:-$REPO_ROOT/build/package}"

usage() {
  cat <<'EOF'
小兔进销存 · macOS 打包

用法：
  tools/package/make_dmg.sh

环境变量：
  XIAOTU_SIGN_IDENTITY  签名证书名称；为空则 ad-hoc 签名
  XIAOTU_NOTARY_PROFILE notarytool keychain profile；为空则跳过公证并打印日志
  XIAOTU_DIST_DIR       产物目录（默认 <repo>/build/package）
  XIAOTU_BUILD          构建号（纯数字，单调递增）；必填，缺失退出 1
  XIAOTU_UPDATE_MANIFEST_URL  单机版清单地址（https）
  XIAOTU_UPDATE_FEED_URL      Sparkle appcast 地址（https）
  XIAOTU_UPDATE_PUBKEY        Sparkle 公钥（base64，解码后 32 字节）
                             更新三项必须同时提供或同时留空

依赖：macOS、CMake 3.25+、Ninja、ccache、Xcode Command Line Tools、hdiutil。
EOF
}

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  "") ;;
  *) echo "未知参数：$1（用 --help 查看用法）" >&2; exit 2 ;;
esac

if ! command -v hdiutil >/dev/null 2>&1; then
  echo "未找到 hdiutil，请在 macOS 上执行。" >&2
  exit 1
fi

VERSION="$(tr -d '[:space:]' < "$REPO_ROOT/VERSION" 2>/dev/null)"
if [ -z "$VERSION" ]; then
  echo "无法从仓库根 VERSION 文件读取版本号。" >&2
  exit 1
fi

UPDATE_MANIFEST_URL="${XIAOTU_UPDATE_MANIFEST_URL:-}"
UPDATE_FEED_URL="${XIAOTU_UPDATE_FEED_URL:-}"
UPDATE_PUBKEY="${XIAOTU_UPDATE_PUBKEY:-}"
UPDATE_CONFIG_COUNT=0
[ -n "$UPDATE_MANIFEST_URL" ] && UPDATE_CONFIG_COUNT=$((UPDATE_CONFIG_COUNT + 1))
[ -n "$UPDATE_FEED_URL" ] && UPDATE_CONFIG_COUNT=$((UPDATE_CONFIG_COUNT + 1))
[ -n "$UPDATE_PUBKEY" ] && UPDATE_CONFIG_COUNT=$((UPDATE_CONFIG_COUNT + 1))

if [ "$UPDATE_CONFIG_COUNT" -ne 0 ] && [ "$UPDATE_CONFIG_COUNT" -ne 3 ]; then
  echo "更新配置不完整：XIAOTU_UPDATE_MANIFEST_URL / XIAOTU_UPDATE_FEED_URL / XIAOTU_UPDATE_PUBKEY 必须同时提供或同时留空。" >&2
  exit 1
fi
if [ "$UPDATE_CONFIG_COUNT" -eq 3 ]; then
  case "$UPDATE_MANIFEST_URL" in
    https://*) ;;
    *)
      echo "XIAOTU_UPDATE_MANIFEST_URL 必须是 https 地址：$UPDATE_MANIFEST_URL" >&2
      exit 1
      ;;
  esac
  case "$UPDATE_FEED_URL" in
    https://*) ;;
    *)
      echo "XIAOTU_UPDATE_FEED_URL 必须是 https 地址：$UPDATE_FEED_URL" >&2
      exit 1
      ;;
  esac
  if [ "$(printf '%s' "$UPDATE_PUBKEY" | base64 -D 2>/dev/null | wc -c | tr -d ' ')" != "32" ]; then
    echo "XIAOTU_UPDATE_PUBKEY 不是合法的 Ed25519 公钥（base64，解码后应为 32 字节）。" >&2
    exit 1
  fi
fi

BUILD_NUMBER="${XIAOTU_BUILD:-}"
if [ -z "$BUILD_NUMBER" ]; then
  echo "打包必须显式设置 XIAOTU_BUILD（纯数字、单调递增），例如：" >&2
  echo "  XIAOTU_BUILD=$(date -u +%Y%m%d%H) tools/package/make_dmg.sh" >&2
  exit 1
fi
case "$BUILD_NUMBER" in
  *[!0-9]*)
    echo "XIAOTU_BUILD 必须是纯数字：$BUILD_NUMBER" >&2
    exit 1
    ;;
esac

# 显式传入（含空值），避免 CMake 缓存把上一次的更新配置带到本次包。
CMAKE_ARGS=(
  --preset macos-release
  -DXIAOTU_BUILD_SHELL=ON
  -DXIAOTU_RELEASE_BUILD=ON
  "-DXIAOTU_BUILD=$BUILD_NUMBER"
  "-DXIAOTU_UPDATE_MANIFEST_URL=$UPDATE_MANIFEST_URL"
  "-DXIAOTU_UPDATE_FEED_URL=$UPDATE_FEED_URL"
  "-DXIAOTU_UPDATE_PUBKEY=$UPDATE_PUBKEY"
)
echo "===== 构建（macos-release，版本 ${VERSION}）"
echo "构建号：${BUILD_NUMBER}（来自 XIAOTU_BUILD）"
cmake "${CMAKE_ARGS[@]}"
cmake --build --preset release

if [ ! -d "$BUILT_APP" ]; then
  echo "构建产物不存在：$BUILT_APP" >&2
  echo "请确认配置时未关闭 -DXIAOTU_BUILD_SHELL（本脚本仅支持 macOS 单机版壳）。" >&2
  exit 1
fi

PLIST="$BUILT_APP/Contents/Info.plist"
PLIST_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$PLIST")"
if [ "$PLIST_VERSION" != "$VERSION" ]; then
  echo "Info.plist 版本（${PLIST_VERSION}）与项目版本（${VERSION}）不一致。" >&2
  exit 1
fi

PLIST_BUILD="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$PLIST")"
if [ "$PLIST_BUILD" != "$BUILD_NUMBER" ]; then
  echo "Info.plist 构建号（${PLIST_BUILD}）与 XIAOTU_BUILD（${BUILD_NUMBER}）不一致。" >&2
  exit 1
fi

if [ "$UPDATE_CONFIG_COUNT" -eq 3 ]; then
  for entry in "XiaotuUpdateManifestURL=$UPDATE_MANIFEST_URL" \
               "SUFeedURL=$UPDATE_FEED_URL" \
               "SUPublicEDKey=$UPDATE_PUBKEY"; do
    key="${entry%%=*}"
    expected="${entry#*=}"
    actual="$(/usr/libexec/PlistBuddy -c "Print :${key}" "$PLIST")"
    if [ "$actual" != "$expected" ]; then
      echo "Info.plist ${key} 注入失败：期望 ${expected}，实际 ${actual}" >&2
      exit 1
    fi
  done
  echo "更新配置：已注入清单地址、appcast 地址与公钥"
else
  echo "警告：未提供更新配置，打出的包更新功能为关闭状态（应用内 enabled=false）。" >&2
fi

STAGE="$OUT_DIR/dmg-root"
DMG="$OUT_DIR/xiaotu-inventory-$VERSION.dmg"

echo "===== 准备 dmg 内容"
rm -rf "$STAGE"
mkdir -p "$STAGE"
ditto "$BUILT_APP" "$STAGE/$APP_DISPLAY_NAME.app"
ln -s /Applications "$STAGE/应用程序"

echo "===== 签名"
if [ -n "${XIAOTU_SIGN_IDENTITY:-}" ]; then
  echo "使用证书：$XIAOTU_SIGN_IDENTITY"
  codesign --force --deep --options runtime --timestamp \
    --sign "$XIAOTU_SIGN_IDENTITY" "$STAGE/$APP_DISPLAY_NAME.app"
else
  echo "未提供 XIAOTU_SIGN_IDENTITY，使用 ad-hoc 签名（不满足 Gatekeeper 与公证要求）"
  codesign --force --deep --sign - "$STAGE/$APP_DISPLAY_NAME.app"
fi
codesign --verify --deep --strict "$STAGE/$APP_DISPLAY_NAME.app"

echo "===== 生成 dmg"
mkdir -p "$OUT_DIR"
rm -f "$DMG"
hdiutil create -volname "$VOLUME_NAME" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
hdiutil verify "$DMG"

echo "===== 公证"
NOTARIZED=0
if [ -n "${XIAOTU_NOTARY_PROFILE:-}" ]; then
  echo "使用 notarytool 凭据：$XIAOTU_NOTARY_PROFILE"
  xcrun notarytool submit "$DMG" --keychain-profile "$XIAOTU_NOTARY_PROFILE" --wait
  xcrun stapler staple "$DMG"
  NOTARIZED=1
else
  echo "跳过：未提供 XIAOTU_NOTARY_PROFILE（notarytool 凭据），dmg 未公证"
fi

if [ -z "${XIAOTU_SIGN_IDENTITY:-}" ]; then
  echo "警告：当前产物为未签名（ad-hoc），distributable 需 Developer ID 签名并公证。" >&2
fi

SIZE_BYTES="$(stat -f %z "$DMG")"
SHA256="$(shasum -a 256 "$DMG" | awk '{print $1}')"

echo
echo "===== 完成"
echo "产物：$DMG"
echo "字节数：$SIZE_BYTES"
echo "SHA256：$SHA256"
echo "签名：$([ -n "${XIAOTU_SIGN_IDENTITY:-}" ] && echo "$XIAOTU_SIGN_IDENTITY" || echo "ad-hoc（不可直接分发）")"
echo "公证：$([ "$NOTARIZED" -eq 1 ] && echo "已公证" || echo "未公证")"
echo "产品版本：$VERSION"
echo "构建号：$PLIST_BUILD"
echo "更新配置：$([ "$UPDATE_CONFIG_COUNT" -eq 3 ] && echo "已注入" || echo "未配置（更新功能关闭）")"
