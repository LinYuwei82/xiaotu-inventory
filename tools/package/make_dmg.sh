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
#   - 设置 XIAOTU_SIGN_IDENTITY 后改用 Developer ID Application 证书签名。
#   本仓库不包含公证流程；未签名/未公证的包首次打开会被 Gatekeeper 拦截，
#   处理方式见 tools/package/README.md。
#
# 环境变量：
#   XIAOTU_SIGN_IDENTITY  签名证书名称（默认空，使用 ad-hoc）
#   XIAOTU_DIST_DIR       产物目录（默认 <repo>/build/package）

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
  XIAOTU_DIST_DIR       产物目录（默认 <repo>/build/package）

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

VERSION="$(sed -nE 's/^project\(xiaotu_inventory VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' \
  "$REPO_ROOT/CMakeLists.txt" | head -1)"
if [ -z "$VERSION" ]; then
  echo "无法从 CMakeLists.txt 解析版本号。" >&2
  exit 1
fi

echo "===== 构建（macos-release，版本 ${VERSION}）"
# 显式打开单机版壳，避免复用之前 -DXIAOTU_BUILD_SHELL=OFF 的构建目录。
cmake --preset macos-release -DXIAOTU_BUILD_SHELL=ON
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

SIZE_BYTES="$(stat -f %z "$DMG")"
SHA256="$(shasum -a 256 "$DMG" | awk '{print $1}')"

echo
echo "===== 完成"
echo "产物：$DMG"
echo "字节数：$SIZE_BYTES"
echo "SHA256：$SHA256"
