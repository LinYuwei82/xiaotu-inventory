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
# dmg 卷名用英文：Finder 挂载路径 /Volumes/<卷名> 对中文卷名在部分环境
# （TCC 沙箱/自动化）下 hdiutil 会报「操作不被允许」，英文卷名无此问题
VOLUME_NAME="Xiaotu Inventory"
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
XIAOTU_SPARKLE_DIR_VAL="${XIAOTU_SPARKLE_DIR:-}"
UPDATE_CONFIG_COUNT=0
[ -n "$UPDATE_MANIFEST_URL" ] && UPDATE_CONFIG_COUNT=$((UPDATE_CONFIG_COUNT + 1))
[ -n "$UPDATE_FEED_URL" ] && UPDATE_CONFIG_COUNT=$((UPDATE_CONFIG_COUNT + 1))
[ -n "$UPDATE_PUBKEY" ] && UPDATE_CONFIG_COUNT=$((UPDATE_CONFIG_COUNT + 1))

if [ -n "$XIAOTU_SPARKLE_DIR_VAL" ] && [ ! -d "$XIAOTU_SPARKLE_DIR_VAL/Sparkle.framework" ]; then
  echo "XIAOTU_SPARKLE_DIR=$XIAOTU_SPARKLE_DIR_VAL 下没有 Sparkle.framework" >&2
  exit 1
fi

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
  "-DXIAOTU_SPARKLE_DIR=$XIAOTU_SPARKLE_DIR_VAL"
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

# Sparkle 必须随 .app 一起分发：可执行文件链接的是 @rpath/Sparkle.framework，
# 未嵌入时在其他机器上会因 dyld 找不到库而直接崩溃。
if [ -n "$XIAOTU_SPARKLE_DIR_VAL" ]; then
  echo "===== 嵌入 Sparkle.framework"
  mkdir -p "$STAGE/$APP_DISPLAY_NAME.app/Contents/Frameworks"
  ditto "$XIAOTU_SPARKLE_DIR_VAL/Sparkle.framework" \
        "$STAGE/$APP_DISPLAY_NAME.app/Contents/Frameworks/Sparkle.framework"
  ls -d "$STAGE/$APP_DISPLAY_NAME.app/Contents/Frameworks/Sparkle.framework" >/dev/null
  echo "已嵌入：Contents/Frameworks/Sparkle.framework"
fi

# 签名配置：entitlements plist（禁用 sandbox，Sparkle 更新要求 bundle 可写）
ENTITLEMENTS="$REPO_ROOT/tools/package/entitlements.plist"
if [ ! -f "$ENTITLEMENTS" ]; then
  echo "entitlements plist 不存在：$ENTITLEMENTS" >&2
  exit 1
fi

# 新版 macOS codesign 不再推荐 --deep 一把梭：子框架如果自己已签，--deep 重签会产生冲突。
# 正确顺序：先签所有子 framework / dylib（嵌套签名），再签 .app 主体。
echo "===== 签名（分步嵌套签名）"
APP_PATH="$STAGE/$APP_DISPLAY_NAME.app"

# 签名前先去掉扩展属性（FinderInfo 会导致 codesign 报 "resource fork" 拒绝）
xattr -cr "$APP_PATH"
xattr -d com.apple.FinderInfo "$APP_PATH" 2>/dev/null || true
xattr -d "com.apple.fileprovider.fpfs#P" "$APP_PATH" 2>/dev/null || true

sign_identity="${XIAOTU_SIGN_IDENTITY:-}"
if [ -n "$sign_identity" ]; then
  echo "使用证书：$sign_identity"
  sign_mode="Developer ID"
else
  echo "未提供 XIAOTU_SIGN_IDENTITY，使用 ad-hoc 签名（不满足 Gatekeeper 与公证要求）"
  sign_identity="-"
  sign_mode="ad-hoc"
fi

# Step 1: 签 Sparkle.framework（用 --deep 覆盖内部 Updater.app、XPCServices 等嵌套 bundle）
SPARKLE_FW="$APP_PATH/Contents/Frameworks/Sparkle.framework"
if [ -d "$SPARKLE_FW" ]; then
  echo "  签 Sparkle.framework（--deep 覆盖嵌套 bundle）..."
  codesign --force --sign "$sign_identity" --options runtime --timestamp \
    --entitlements "$ENTITLEMENTS" --deep "$SPARKLE_FW"
  codesign --verify --deep --strict "$SPARKLE_FW" || { echo "Sparkle.framework 签名验证失败"; exit 1; }
fi

# Step 2: 签 .app 主体（不使用 --deep，避免重签已签过的子框架）
echo "  签 .app 主体..."
codesign --force --sign "$sign_identity" --options runtime --timestamp \
  --entitlements "$ENTITLEMENTS" "$APP_PATH"

# 整体验证
codesign --verify --deep --strict "$APP_PATH"
spctl --assess --type execute -vv "$APP_PATH" >/dev/null 2>&1 && echo "  spctl 预检：accepted ✓" || echo "  spctl 预检：$(spctl --assess --type execute -vv "$APP_PATH" 2>&1 || true)"

echo "===== 生成 dmg"
mkdir -p "$OUT_DIR"
rm -f "$DMG"
hdiutil create -volname "$VOLUME_NAME" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
hdiutil verify "$DMG"

# DMG 也签名（Developer ID 签名的 DMG 让 mount 过程更顺畅）
if [ -n "${XIAOTU_SIGN_IDENTITY:-}" ]; then
  codesign --force --sign "$sign_identity" --timestamp "$DMG"
fi

echo "===== 公证"
NOTARIZED=0
NOTARY_PROFILE="${XIAOTU_NOTARY_PROFILE:-}"
if [ -n "$NOTARY_PROFILE" ]; then
  # 提前检测 profile 是否在 Keychain 中存在
  if xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" 2>&1 | head -1 | grep -qi "error"; then
    echo "错误：notarytool keychain profile '$NOTARY_PROFILE' 不存在" >&2
    echo "      需要先运行：" >&2
    echo "        xcrun notarytool store-credentials '$NOTARY_PROFILE' \\" >&2
    echo "          --apple-id <your@appleid.com> \\" >&2
    echo "          --team-id BMCUC4B6XX \\" >&2
    echo "          --password <app-specific-password>" >&2
    exit 1
  fi
  echo "使用 notarytool 凭据：$NOTARY_PROFILE"
  xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait \
    || { echo "notarytool submit 失败"; exit 1; }
  xcrun stapler staple "$DMG"
  NOTARIZED=1
else
  echo "跳过：未提供 XIAOTU_NOTARY_PROFILE（notarytool 凭据），dmg 未公证"
  echo "      要开启公证，先在 Keychain 存一次凭据："
  echo "        xcrun notarytool store-credentials 'xiaotu-notary' \\"
  echo "          --apple-id <your@appleid.com> --team-id BMCUC4B6XX --password <app-specific-password>"
  echo "      然后重新跑：XIAOTU_NOTARY_PROFILE=xiaotu-notary tools/package/make_dmg.sh"
fi

if [ -z "${XIAOTU_SIGN_IDENTITY:-}" ]; then
  echo "警告：当前产物为 ad-hoc 签名，distributable 需 Developer ID 签名并公证。" >&2
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
