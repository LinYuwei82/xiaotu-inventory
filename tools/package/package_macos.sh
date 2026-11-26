#!/usr/bin/env bash
# 小兔进销存 · macOS 可分发包（.dmg）
#
# 一条命令从构建到出 dmg：
#   tools/package/package_macos.sh
#
# 产物：dist/xiaotu-inventory-<版本>.dmg
#   ├── 小兔进销存.app
#   └── 应用程序 -> /Applications
#
# 签名与公证（硬性前提，2026-11-20 起生效）：
#   - 必须提供 XIAOTU_SIGN_IDENTITY（Developer ID Application 证书名称），否则 exit 1
#   - 必须提供 XIAOTU_NOTARY_PROFILE（notarytool keychain profile 名），否则 exit 1
#   - 已移除 ad-hoc 签名与跳过公证的旁路（历史上用于本地调试，发布路径禁止）
#   - 签名口径见 D13：Developer ID Application 逐层签名 + 公证（notarytool + stapler）
#
# 环境变量：
#   XIAOTU_SIGN_IDENTITY  Developer ID Application 证书名称；必填
#   XIAOTU_NOTARY_PROFILE notarytool keychain profile；必填
#   XIAOTU_DIST_DIR       产物目录（默认 <repo>/dist）
#   XIAOTU_BUILD          构建号（纯数字）；发布时传 tag 序号，未传回退 gen_build.py
#   XIAOTU_UPDATE_MANIFEST_URL  单机版清单地址（https）
#   XIAOTU_UPDATE_FEED_URL      Sparkle appcast 地址（https）
#   XIAOTU_UPDATE_PUBKEY        Sparkle 公钥（Ed25519，base64，32 字节）
#   更新三项必须同时提供或同时留空；留空表示更新功能关闭。
#   XIAOTU_SPARKLE_DIR         可选；留空使用仓库内 third_party/sparkle（发布构建必带 Sparkle）

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

APP_DISPLAY_NAME="小兔进销存"
# dmg 卷名用英文：Finder 挂载路径 /Volumes/<卷名> 对中文卷名在部分环境
# （TCC 沙箱/自动化）下 hdiutil 会报「操作不被允许」，英文卷名无此问题
VOLUME_NAME="Xiaotu Inventory"
BUILD_DIR="$REPO_ROOT/build/release"
BUILT_APP="$BUILD_DIR/shell/xiaotu-inventory.app"
OUT_DIR="${XIAOTU_DIST_DIR:-$REPO_ROOT/dist}"

usage() {
  cat <<'EOF'
小兔进销存 · macOS 打包

用法：
  XIAOTU_SIGN_IDENTITY="Developer ID Application: <你的名字>" \
  XIAOTU_NOTARY_PROFILE=xiaotu-notary \
  tools/package/package_macos.sh

硬性前提（2026-11-20 起生效）：签名证书 + 公证 profile 必须同时提供，缺失则 exit 1。

环境变量：
  XIAOTU_SIGN_IDENTITY  Developer ID Application 证书名称；必填
  XIAOTU_NOTARY_PROFILE notarytool keychain profile；必填
  XIAOTU_DIST_DIR       产物目录（默认 <repo>/dist）
  XIAOTU_BUILD          构建号（纯数字）；发布时传 tag 序号，未传回退 gen_build.py
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


VERSION="$(tr -d '[:space:]' < "$REPO_ROOT/VERSION.txt" 2>/dev/null)"
if [ -z "$VERSION" ]; then
  echo "无法从仓库根 VERSION.txt 读取版本号。" >&2
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

# 构建号：正式发布传 tag 序号（build = seq，见 tools/release/release_tag.py）。
# 未传时回退 gen_build.py 生成的内测构建号，仅用于本地验证。
if [ -n "${XIAOTU_BUILD:-}" ]; then
  BUILD_NUMBER="$XIAOTU_BUILD"
  case "$BUILD_NUMBER" in
    *[!0-9]*) echo "XIAOTU_BUILD 必须是纯数字：$BUILD_NUMBER" >&2; exit 1 ;;
  esac
  echo "构建号：${BUILD_NUMBER}（来源 XIAOTU_BUILD 环境变量）"
else
  BUILD_NUMBER="$(python3 "$REPO_ROOT/tools/release/gen_build.py" \
    --product xiaotu --repo-root "$REPO_ROOT")" || {
    echo "gen_build.py 失败，无法生成构建号" >&2; exit 1; }
  echo "构建号：${BUILD_NUMBER}（来源 gen_build.py）"
fi

# ===== 硬门禁前置：签名证书 + 公证 profile 必须同时满足 =====
if [ -z "${XIAOTU_SIGN_IDENTITY:-}" ]; then
  echo "硬性前提检查失败：XIAOTU_SIGN_IDENTITY 未设置" >&2
  echo "  需要 Developer ID Application 证书名称" >&2
  echo "  钥匙串查找：security find-identity -v -p codesigning" >&2
  echo "  然后：export XIAOTU_SIGN_IDENTITY='Developer ID Application: <你的名字>'" >&2
  exit 1
fi
echo "签名证书：${XIAOTU_SIGN_IDENTITY} ✓"

if [ -z "${XIAOTU_NOTARY_PROFILE:-}" ]; then
  echo "硬性前提检查失败：XIAOTU_NOTARY_PROFILE 未设置" >&2
  echo "  需要 notarytool keychain profile 名" >&2
  echo "  先存储一次：xcrun notarytool store-credentials 'xiaotu-notary' \\" >&2
  echo "    --apple-id <your@appleid.com> --team-id BMCUC4B6XX --password <app-specific-password>" >&2
  echo "  然后：export XIAOTU_NOTARY_PROFILE=xiaotu-notary" >&2
  exit 1
fi
# profile 存在性验证
if xcrun notarytool history --keychain-profile "$XIAOTU_NOTARY_PROFILE" 2>&1 | head -1 | grep -qi "error"; then
  echo "硬性前提检查失败：notarytool keychain profile '$XIAOTU_NOTARY_PROFILE' 不存在" >&2
  echo "  重新存储：xcrun notarytool store-credentials '$XIAOTU_NOTARY_PROFILE' \\" >&2
  echo "    --apple-id <your@appleid.com> --team-id BMCUC4B6XX --password <app-specific-password>" >&2
  exit 1
fi
echo "公证 profile：${XIAOTU_NOTARY_PROFILE} ✓"

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
echo "构建号：${BUILD_NUMBER}（本次使用）"
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

# 中间产物放到临时目录：dist/ 里只保留最终 dmg（清单也落在这里）
STAGE="$(mktemp -d -t xiaotu-dmg-root)"
trap 'rm -rf "$STAGE"' EXIT
DMG="$OUT_DIR/xiaotu-inventory-$VERSION.dmg"

echo "===== 准备 dmg 内容"
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
# 发布包不允许缺少 Sparkle：否则安装会退化为「打开 dmg 让用户拖拽」。
if [ ! -d "$STAGE/$APP_DISPLAY_NAME.app/Contents/Frameworks/Sparkle.framework" ]; then
  echo "安装包缺少 Sparkle.framework：自动更新会退化为打开 dmg，拒绝出包。" >&2
  echo "  仓库内应有 third_party/sparkle/Sparkle.framework，或用 XIAOTU_SPARKLE_DIR 指定。" >&2
  exit 1
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

sign_identity="$XIAOTU_SIGN_IDENTITY"
echo "使用证书：${sign_identity}（硬性前提已前置检查 ✓）"

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

# .app 也做一次公证 + staple：用户把 .app 拖进 /Applications 后首启不再依赖在线核验，
# 避免断网/代理拦截导致 Gatekeeper 弹「无法验证」弹窗。
# 顺序必须是「先公证 .app → staple .app → 再打 dmg」，这样 dmg 里装的是已 staple 的 .app。
# notarytool 只收 zip/pkg/dmg：用 ditto --keepParent 把 .app 压成 zip 提交，
# 票据按二进制 cdhash 匹配，staple 仍打进 .app 本体。
echo "===== 公证 .app（离线首启）"
echo "使用 notarytool 凭据：${XIAOTU_NOTARY_PROFILE}（硬性前提已前置检查 ✓）"
APP_ZIP="$(mktemp -t xiaotu-app-notarize).zip"
ditto -c -k --keepParent "$APP_PATH" "$APP_ZIP"
xcrun notarytool submit "$APP_ZIP" --keychain-profile "$XIAOTU_NOTARY_PROFILE" --wait \
  || { echo "notarytool submit .app.zip 失败"; rm -f "$APP_ZIP"; exit 1; }
rm -f "$APP_ZIP"
xcrun stapler staple "$APP_PATH"
xcrun stapler validate "$APP_PATH"
echo ".app 公证与 staple 通过 ✓"

echo "===== 生成 dmg"
mkdir -p "$OUT_DIR"
rm -f "$DMG"
hdiutil create -volname "$VOLUME_NAME" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
hdiutil verify "$DMG"

# DMG 也签名（Developer ID 签名的 DMG 让 mount 过程更顺畅，公证更稳）
codesign --force --sign "$sign_identity" --timestamp "$DMG"

echo "===== 公证"
echo "使用 notarytool 凭据：${XIAOTU_NOTARY_PROFILE}（硬性前提已前置检查 ✓）"
xcrun notarytool submit "$DMG" --keychain-profile "$XIAOTU_NOTARY_PROFILE" --wait \
  || { echo "notarytool submit 失败"; exit 1; }
xcrun stapler staple "$DMG"
xcrun stapler validate "$DMG"
echo "公证通过 ✓"

SIZE_BYTES="$(stat -f %z "$DMG")"
SHA256="$(shasum -a 256 "$DMG" | awk '{print $1}')"

echo
echo "===== 完成"
echo "产物：$DMG"
echo "字节数：$SIZE_BYTES"
echo "SHA256：$SHA256"
echo "签名：${XIAOTU_SIGN_IDENTITY}（Developer ID ✓）"
echo "公证：${XIAOTU_NOTARY_PROFILE}（notarytool 已公证 ✓）"
echo "app staple：已注入 ✓"
echo "dmg staple：已注入 ✓"
echo "产品版本：$VERSION"
echo "构建号：$PLIST_BUILD"
echo "更新配置：$([ "$UPDATE_CONFIG_COUNT" -eq 3 ] && echo "已注入" || echo "未配置（更新功能关闭）")"
GIT_HASH="$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")"
echo "commit：$GIT_HASH"
