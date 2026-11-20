#!/usr/bin/env bash
# 小兔在线 · OSS 发布脚本（自动更新链路，更新清单 Schema v2）
#
# 输入 dmg → 生成 Ed25519 签名的 manifest.json（v2）+ appcast.xml → 上传 dl.xiaotu.online（OSS 直连，无 CDN）。
# 只负责小兔进销存；DevCard / Mote 各有自己的发布脚本。
#
# 目录结构：
#   oss://<bucket>/xiaotu/xiaotu-<ver>-macos-arm64.dmg            ← 长缓存 / immutable（v2 命名）
#   oss://<bucket>/xiaotu/updates/<channel>/manifest.json         ← 60 秒缓存（v2 清单）
#   oss://<bucket>/xiaotu/updates/<channel>/appcast.xml           ← 60 秒缓存（Sparkle，不在 v2 范围）
#
# 清单生成/签名使用三仓库共用的 tools/release/manifest_v2.py（纯标准库），
# schema 见 mote/docs/更新清单Schema-v2.md；本脚本不再生成 latest.json，
# 旧内测客户端按切换方案重装或经 Sparkle（appcast.xml）升级。
#
# 下载页（pre.xiaotu.online/download）由 _site 桶的 site.js 读清单自动同步，
# 本脚本不再改写任何 HTML。
#
# 凭据（AccessKey）：
#   1. 优先读环境变量 OSS_ACCESS_KEY_ID / OSS_ACCESS_KEY_SECRET；
#   2. 未设置环境变量时读仓库外的 ~/.config/xiaotu-updates/oss-credentials.env
#      （两行键值，权限必须为 600 或 400；可用 OSS_CRED_FILE 改路径）。
#   密钥只授予该桶的最小权限，绝不写进仓库、脚本或 CI 日志；若曾以明文出现在
#   聊天、命令行历史或日志里，请立即到 RAM 控制台轮换。发布前脚本会检查受版本
#   控制的文件里是否残留当前 AccessKey，命中即拒绝发布。
#
# 依赖：python3（manifest_v2.py / guard.py 纯标准库）、cryptography venv（仅 appcast 用）、
#       openssl 3.x、shasum、PlistBuddy。openssl 会自动找 brew 的 openssl@3（keg-only，不进 PATH），
#       也可用 OPENSSL_BIN=/path/to/openssl 指定；macOS 自带的 LibreSSL 会被拒绝。
#
# 用法：
#   # 完整发布（需要 OSS 凭据，见上）
#   OSS_BUCKET=xxx OSS_REGION=cn-hangzhou PRODUCT=xiaotu tools/package/publish_oss.sh
#
#   # dry-run：只本地生成 + 验签，不碰 OSS
#   OSS_SKIP=true PRODUCT=xiaotu tools/package/publish_oss.sh
#
#   # 只补发说明页：上传 docs/release-notes-<版本>.html，并用 resign 重签线上清单
#   # （不重出包、不改版本与 dmg；可用 NOTES_VERSION 指定历史版本）
#   NOTES_ONLY=true tools/package/publish_oss.sh
#
#   # 关闭发布后的下载可达性验证
#   WARMUP=false PRODUCT=xiaotu tools/package/publish_oss.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

VERSION="$(tr -d '[:space:]' < "$REPO_ROOT/VERSION.txt" 2>/dev/null)"
[ -n "$VERSION" ] || { echo "无法从仓库根 VERSION.txt 读取版本号。" >&2; exit 1; }

usage() {
  sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}
case "${1:-}" in -h|--help) usage; exit 0 ;; "") ;;
  *) echo "未知参数：$1（用 --help 查看用法）" >&2; exit 2 ;; esac

# ---- 核心参数 ----
PRODUCT="${PRODUCT:-xiaotu}"
PRODUCT_DIR="$PRODUCT"   # OSS 对象键前缀（windows.json 时代沿用名）
case "$PRODUCT" in
  xiaotu) ;;
  devcard|mote)
    echo "PRODUCT=$PRODUCT 已迁出本仓库：DevCard 用 dev_card_cpp/tools/release/publish_macos.sh，Mote 用 mote/tools/release/publish_macos.sh" >&2
    exit 1 ;;
  *) echo "PRODUCT 必须是 xiaotu" >&2; exit 1 ;;
esac

# ---- 配置文件：环境变量优先，其次仓库外 600 权限的凭据文件 ----
OSS_CRED_FILE="${OSS_CRED_FILE:-$HOME/.config/xiaotu-updates/oss-credentials.env}"
# 兼容只有 .env 的环境（内容为 export KEY=VALUE）
if [ ! -f "$OSS_CRED_FILE" ] && [ -f "$HOME/.config/xiaotu-updates/.env" ]; then
  OSS_CRED_FILE="$HOME/.config/xiaotu-updates/.env"
fi

cred_value() {  # cred_value <文件> <键名>
  sed -n "s/^[[:space:]]*\(export[[:space:]]\{1,\}\)\{0,1\}$2[[:space:]]*=[[:space:]]*//p" "$1" | tail -1 \
    | tr -d '\r' | sed -e 's/^"//' -e 's/"$//' -e "s/^'//" -e "s/'$//"
}

OSS_BUCKET="${OSS_BUCKET:-}"
OSS_REGION="${OSS_REGION:-}"
# 未通过环境变量提供时，从凭据文件读取桶名与区域（与 Windows 发布脚本行为一致）
if [ -f "$OSS_CRED_FILE" ]; then
  [ -n "$OSS_BUCKET" ] || OSS_BUCKET="$(cred_value "$OSS_CRED_FILE" OSS_BUCKET)"
  [ -n "$OSS_REGION" ] || OSS_REGION="$(cred_value "$OSS_CRED_FILE" OSS_REGION)"
fi
CDN_DOMAIN="${CDN_DOMAIN:-https://dl.xiaotu.online}"
OSS_SKIP="${OSS_SKIP:-false}"
# 只补发发行说明并重签清单：不要求 dmg，也不改版本、产物与 appcast。
NOTES_ONLY="${NOTES_ONLY:-false}"

if [ "$OSS_SKIP" != "true" ]; then
  if [ -z "$OSS_BUCKET" ] || [ -z "$OSS_REGION" ]; then
    echo "缺少必填：OSS_BUCKET / OSS_REGION" >&2
    echo "可用环境变量传入，或写入 ${OSS_CRED_FILE}（OSS_BUCKET= / OSS_REGION=）" >&2
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
if [ "$NOTES_ONLY" != "true" ]; then
  [ -f "$DMG_PATH" ] || { echo "dmg 不存在：$DMG_PATH" >&2; exit 1; }
fi

DEFAULT_KEY="$HOME/.config/xiaotu-updates/update_signing_2026.pem"
SIGNING_KEY="${SIGNING_KEY:-$DEFAULT_KEY}"

# ---- openssl 解析：必须是真 OpenSSL 3.x，不能落到 macOS 自带的 LibreSSL ----
# Homebrew 的 openssl@3 是 keg-only、不会进 PATH，所以优先用 brew prefix；
# 也支持 OPENSSL_BIN 显式指定。（openssl 仅 Sparkle 公钥换算等用途；清单签名由 manifest_v2.py 完成）
resolve_openssl_bin() {
  if [ -n "${OPENSSL_BIN:-}" ]; then printf '%s\n' "$OPENSSL_BIN"; return 0; fi
  if command -v brew >/dev/null 2>&1; then
    local prefix
    prefix="$(brew --prefix openssl@3 2>/dev/null || true)"
    if [ -n "$prefix" ] && [ -x "$prefix/bin/openssl" ]; then
      printf '%s\n' "$prefix/bin/openssl"; return 0
    fi
  fi
  command -v openssl 2>/dev/null
}
OPENSSL_BIN="$(resolve_openssl_bin || true)"

[ -f "$SIGNING_KEY" ] || {
  echo "签名私钥不存在：$SIGNING_KEY" >&2
  echo "用 ${OPENSSL_BIN:-openssl} genpkey -algorithm ED25519 -out $SIGNING_KEY 生成" >&2; exit 1; }
SIGNING_PUB="${SIGNING_KEY%.pem}.pub.pem"

KEY_ID="${KEY_ID:-2026-11}"
SCHEMA_MIN="${SCHEMA_MIN:-1}"
SCHEMA_MAX="${SCHEMA_MAX:-39}"
API_MIN="${API_MIN:-1}"
API_MAX="${API_MAX:-1}"
MANDATORY="${MANDATORY:-}"
RELEASE_NOTES_URL="${RELEASE_NOTES_URL:-}"
RELEASE_NOTES_FILE="${RELEASE_NOTES_FILE:-}"
# dl.xiaotu.online 是 OSS 自定义域名（CNAME 直连 OSS，无 CDN 层），源站上传即生效，
# 无需刷新边缘缓存；保留 PURGE 开关仅备将来接入 CDN（PURGE=true 开启）。
PURGE="${PURGE:-false}"
# 发布后下载一次 dmg 做可达性验证（WARMUP=false 关闭，WARM_TIMEOUT 单位秒）
WARMUP="${WARMUP:-true}"
WARM_TIMEOUT="${WARM_TIMEOUT:-300}"

# ---- OSS 凭据：环境变量优先，其次上面的凭据文件 ----
# 凭据只在本进程内使用，不导出给子进程：上传时按参数传给 Python。
mask_ak() {  # 只露前 8 位，便于确认用的是哪把密钥
  local key="${1:-}"
  if [ "${#key}" -le 8 ]; then printf '****'; else printf '%s****' "${key:0:8}"; fi
}

load_oss_credentials() {
  if [ -n "${OSS_ACCESS_KEY_ID:-}" ] && [ -n "${OSS_ACCESS_KEY_SECRET:-}" ]; then
    OSS_CRED_SOURCE="环境变量"
  else
    if [ ! -f "$OSS_CRED_FILE" ]; then
      echo "缺少 OSS 凭据：请设置 OSS_ACCESS_KEY_ID / OSS_ACCESS_KEY_SECRET，" >&2
      echo "或写入仓库外的 ${OSS_CRED_FILE}（权限 600）。" >&2
      exit 1
    fi
    local perm
    perm="$(stat -c %a "$OSS_CRED_FILE" 2>/dev/null || stat -f %Lp "$OSS_CRED_FILE" 2>/dev/null || echo unknown)"
    case "$perm" in
      600|400) ;;
      *) echo "凭据文件权限过宽（${perm}）：先 chmod 600 $OSS_CRED_FILE 再重试。" >&2; exit 1 ;;
    esac
    OSS_ACCESS_KEY_ID="$(cred_value "$OSS_CRED_FILE" OSS_ACCESS_KEY_ID)"
    OSS_ACCESS_KEY_SECRET="$(cred_value "$OSS_CRED_FILE" OSS_ACCESS_KEY_SECRET)"
    if [ -z "$OSS_ACCESS_KEY_ID" ] || [ -z "$OSS_ACCESS_KEY_SECRET" ]; then
      echo "凭据文件缺少 OSS_ACCESS_KEY_ID / OSS_ACCESS_KEY_SECRET：$OSS_CRED_FILE" >&2
      exit 1
    fi
    OSS_CRED_SOURCE="$(basename "$OSS_CRED_FILE")"
  fi
  AK_MASK="$(mask_ak "$OSS_ACCESS_KEY_ID")"
}

# 轮换自检：当前 AccessKey 不得出现在受版本控制的文件里，命中即拒绝发布。
assert_cred_not_tracked() {
  git -C "$REPO_ROOT" rev-parse --git-dir >/dev/null 2>&1 || return 0
  if git -C "$REPO_ROOT" grep -qF -- "$OSS_ACCESS_KEY_ID" -- . 2>/dev/null; then
    echo "✗ AccessKey 出现在仓库受版本控制的文件里，已拒绝发布。" >&2
    echo "  请先移除该值，再到 RAM 控制台轮换这对密钥后重跑。" >&2
    exit 1
  fi
}

# ---- 依赖检查 ----
VENV_PY="$REPO_ROOT/tools/package/.venv/bin/python"
if [ ! -x "$VENV_PY" ]; then
  python3 -m venv "$REPO_ROOT/tools/package/.venv"
  "$REPO_ROOT/tools/package/.venv/bin/pip" install -q -r "$REPO_ROOT/tools/package/requirements.txt"
fi
[ -n "$OPENSSL_BIN" ] || {
  echo "未找到 openssl：请 brew install openssl@3，或用 OPENSSL_BIN 指定可执行文件" >&2; exit 1; }
OPENSSL_VER="$("$OPENSSL_BIN" version 2>/dev/null || true)"
case "$OPENSSL_VER" in
  "OpenSSL 3."*) ;;
  *) echo "openssl 不可用：$OPENSSL_BIN → ${OPENSSL_VER:-<无输出>}" >&2
     echo "需要 OpenSSL 3.x；macOS 自带的 LibreSSL 不算" >&2
     exit 1 ;;
esac
command -v shasum >/dev/null 2>&1 || { echo "未找到 shasum" >&2; exit 1; }
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
  load_oss_credentials
  assert_cred_not_tracked
fi

# ---- 元数据：构建号从 gen_build.py 生成；DMG Info.plist 用于校验 + MIN_OS ----
if [ -n "${BUILD:-}" ]; then
  BUILD_SRC="用户显式传 BUILD"
else
  BUILD="$(python3 "$REPO_ROOT/tools/release/gen_build.py" \
    --product xiaotu --repo-root "$REPO_ROOT")" || {
    echo "gen_build.py 失败，无法生成构建号" >&2; exit 1; }
  BUILD_SRC="gen_build.py"
fi
DMG_MOUNT="$(mktemp -d -t xiaotu-dmg)"
if [ "$NOTES_ONLY" != "true" ]; then
hdiutil attach "$DMG_PATH" -mountpoint "$DMG_MOUNT" -nobrowse -readonly >/dev/null 2>&1 || true
APP_IN_DMG="$(find "$DMG_MOUNT" -name "*.app" -maxdepth 2 2>/dev/null | head -1 || true)"
PLIST_VERSION=""
PLIST_BUILD=""
MIN_OS=""
if [ -n "$APP_IN_DMG" ] && [ -f "$APP_IN_DMG/Contents/Info.plist" ]; then
  PLIST_BUILD="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' \
    "$APP_IN_DMG/Contents/Info.plist" 2>/dev/null || true)"
  PLIST_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
    "$APP_IN_DMG/Contents/Info.plist" 2>/dev/null || true)"
  MIN_OS="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' \
    "$APP_IN_DMG/Contents/Info.plist" 2>/dev/null || true)"
  if [ "$PLIST_VERSION" != "$VERSION" ]; then
    echo "警告：dmg 内版本 $PLIST_VERSION ≠ 仓库 VERSION $VERSION" >&2
  fi
  # 校验 DMG 里的 CFBundleVersion 与 gen_build.py 一致
  if [ -n "$PLIST_BUILD" ] && [ "$PLIST_BUILD" != "$BUILD" ]; then
    echo "警告：dmg 内 CFBundleVersion=$PLIST_BUILD ≠ 发布用 build=$BUILD ($BUILD_SRC)" >&2
    echo "  → 确认 DMG 是用相同 build 打包的，否则客户端会反复提示更新" >&2
  fi
fi
hdiutil detach "$DMG_MOUNT" >/dev/null 2>&1 || true
rm -rf "$DMG_MOUNT"

# BUILD 兜底：gen_build.py 已经生成了，不会再为空
[[ "$BUILD" =~ ^[0-9]{1,}$ ]] || { echo "BUILD 非法（$BUILD_SRC）" >&2; exit 1; }
echo "build：${BUILD}（来源 $BUILD_SRC）"

PUBLISHED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
PUB_BASE64="$("$OPENSSL_BIN" pkey -pubin -in "$SIGNING_PUB" -outform DER 2>/dev/null \
  | tail -c 32 | base64 | tr -d '\n')"

# ---- 路径（纯 object key，不含 bucket/endpoint）----
# v2 安装包命名：<product>-<version>-<os>-<arch>-<type>.<ext>（见更新清单Schema-v2.md 第七节）
V2_DMG_NAME="xiaotu-${VERSION}-macos-arm64.dmg"
DMG_KEY="$PRODUCT_DIR/$V2_DMG_NAME"
DMG_PUBLIC_URL="$CDN_DOMAIN/$DMG_KEY"
MANIFEST_KEY="$PRODUCT_DIR/updates/$CHANNEL/manifest.json"
APPCAST_KEY="$PRODUCT_DIR/updates/$CHANNEL/appcast.xml"
MANIFEST_PUBLIC="$CDN_DOMAIN/$MANIFEST_KEY"
APPCAST_PUBLIC="$CDN_DOMAIN/$APPCAST_KEY"

# 发行说明：与 Windows 发布同一约定——优先 docs/release-notes-<版本>.html；
# 没有 .html 但有 .md 时自动用 Python markdown 转 .html（客户端浏览器需要 html 渲染）。
# 也可用 RELEASE_NOTES_FILE 显式指定。
NOTES_KEY="$PRODUCT_DIR/release-notes-$VERSION.html"
# 辅助函数：找 .html，没有则从同名 .md 自动转（html 比 md 旧也重转）
resolve_notes_file() {
  local ver md html
  ver="$1"
  md="$REPO_ROOT/docs/release-notes-$ver.md"
  html="$REPO_ROOT/docs/release-notes-$ver.html"
  if [ ! -f "$html" ] || { [ -f "$md" ] && [ "$md" -nt "$html" ]; }; then
    if [ -f "$md" ]; then
      if ! python3 -c 'import markdown' 2>/dev/null; then
        echo "⚠️  有 release-notes-$ver.md 但缺 Python markdown 库，跳过 md→html" >&2; return 1
      fi
      python3 - "$md" "$html" <<'PYEOF'
import markdown, sys
css = """body{font-family:-apple-system,"PingFang SC","Microsoft YaHei",sans-serif;max-width:720px;margin:40px auto;padding:0 20px;line-height:1.7;color:#222}h1{border-bottom:2px solid #eee;padding-bottom:8px}h2{margin-top:1.5em;color:#111}table{border-collapse:collapse;width:100%;margin:1em 0}th,td{border:1px solid #ddd;padding:8px 12px;text-align:left}th{background:#f6f8fa}code{background:#f0f0f0;padding:1px 6px;border-radius:3px;font-size:.9em}pre{background:#f6f8fa;padding:12px 16px;border-radius:6px;overflow-x:auto}pre code{background:none;padding:0}blockquote{border-left:4px solid #ddd;margin:1em 0;padding:0 16px;color:#666}"""
body = markdown.markdown(open(sys.argv[1],encoding='utf-8').read(), extensions=['tables','fenced_code'])
open(sys.argv[2],'w',encoding='utf-8').write(f'<!DOCTYPE html><html><head><meta charset="utf-8"><style>{css}</style></head><body>{body}</body></html>')
PYEOF
      echo "  (md → html 自动生成 $html)"
    fi
  fi
  [ -f "$html" ] && echo "$html"
}
if [ -z "$RELEASE_NOTES_FILE" ] && [ -z "$RELEASE_NOTES_URL" ]; then
  auto_file="$(resolve_notes_file "$VERSION" || true)"
  [ -n "$auto_file" ] && RELEASE_NOTES_FILE="$auto_file"
fi
if [ -n "$RELEASE_NOTES_FILE" ]; then
  [ -f "$RELEASE_NOTES_FILE" ] || { echo "发行说明不存在：$RELEASE_NOTES_FILE" >&2; exit 1; }
  [ -n "$RELEASE_NOTES_URL" ] || RELEASE_NOTES_URL="$CDN_DOMAIN/$NOTES_KEY"
fi
if [ -n "$RELEASE_NOTES_URL" ]; then
  case "$RELEASE_NOTES_URL" in
    http://*|https://*) ;;
    *) echo "RELEASE_NOTES_URL 必须是绝对 http(s) 地址：$RELEASE_NOTES_URL" >&2; exit 2 ;;
  esac
fi

echo "===== OSS 发布 · ${PRODUCT} ====="
echo "版本 / channel：          ${VERSION} / ${CHANNEL}"
if [ "$NOTES_ONLY" != "true" ]; then
echo "build：                   ${BUILD}"
echo "dmg：                     ${DMG_PATH}"
echo "远端文件名（v2）：        ${V2_DMG_NAME}"
echo "min_os：                  ${MIN_OS:-（空）}"
fi
echo "bucket / region：         ${OSS_BUCKET:-<dry-run>} / ${OSS_REGION:-<dry-run>}"
echo "AK / 来源：               ${AK_MASK:-<dry-run>} / ${OSS_CRED_SOURCE:-<dry-run>}"
echo "域名：                    ${CDN_DOMAIN}"
echo "OSS key："
[ "$NOTES_ONLY" != "true" ] && echo "  dmg：                   ${DMG_KEY}"
echo "  manifest.json：         ${MANIFEST_KEY}"
[ "$NOTES_ONLY" != "true" ] && echo "  appcast.xml：           ${APPCAST_KEY}"
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

MANIFEST_V2_PY="$REPO_ROOT/tools/release/manifest_v2.py"

# ---- 仅补发说明页：上传 notes + resign 重签清单，不碰 dmg / appcast ----
if [ "$NOTES_ONLY" = "true" ]; then
  echo ""
  echo "===== 仅补发说明页"
  NOTES_VERSION="${NOTES_VERSION:-$VERSION}"
  if [ -n "${RELEASE_NOTES_FILE:-}" ]; then
    NOTES_FILE="$RELEASE_NOTES_FILE"
  else
    NOTES_FILE="$(resolve_notes_file "$NOTES_VERSION" || true)"
    [ -z "$NOTES_FILE" ] && NOTES_FILE="$REPO_ROOT/docs/release-notes-$NOTES_VERSION.html"
  fi
  [ -f "$NOTES_FILE" ] || { echo "发行说明不存在：$NOTES_FILE" >&2; exit 1; }
  NOTES_URL="${RELEASE_NOTES_URL:-$CDN_DOMAIN/$PRODUCT_DIR/release-notes-$NOTES_VERSION.html}"
  case "$NOTES_URL" in
    http://*|https://*) ;;
    *) echo "RELEASE_NOTES_URL 必须是绝对 http(s) 地址：$NOTES_URL" >&2; exit 2 ;;
  esac

  LIVE="$STAGE/manifest-live.json"
  echo "拉取线上清单：$CDN_DOMAIN/$MANIFEST_KEY"
  curl -fsSL "$CDN_DOMAIN/$MANIFEST_KEY" -o "$LIVE" \
    || { echo "拉取线上清单失败：$CDN_DOMAIN/$MANIFEST_KEY" >&2; exit 1; }
  python3 - "$LIVE" "$NOTES_VERSION" <<'PY_CHECK_EOF'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
want = sys.argv[2]
versions = {k: v.get("version") for k, v in body.get("platforms", {}).items()}
if not versions or set(versions.values()) != {want}:
    sys.exit(f"线上清单版本 {versions} 与 NOTES_VERSION={want} 不一致，已中止")
print(f"  线上清单版本：{want}（{', '.join(sorted(versions))}）")
PY_CHECK_EOF

  SIGNED="$STAGE/manifest.json"
  echo "重签清单：notes_url=${NOTES_URL}（platform=all）"
  python3 "$MANIFEST_V2_PY" resign --manifest "$LIVE" --key "$SIGNING_KEY" \
    --notes-url "$NOTES_URL" --platform all --out "$SIGNED"
  python3 "$MANIFEST_V2_PY" verify --manifest "$SIGNED" --pub "$SIGNING_PUB"

  if [ "$OSS_SKIP" != "true" ]; then
    echo "上传发行说明 → ${PRODUCT_DIR}/release-notes-${NOTES_VERSION}.html"
    oss_upload "$NOTES_FILE" "${PRODUCT_DIR}/release-notes-${NOTES_VERSION}.html" \
      "public, max-age=3600" "text/html; charset=utf-8"
    echo "上传 manifest.json → $MANIFEST_KEY"
    oss_upload "$SIGNED" "$MANIFEST_KEY" "no-cache, max-age=60" "application/json"
  else
    echo "[dry-run] 未上传；已生成 $SIGNED"
  fi
  echo "完成：仅补发说明页，dmg / appcast 未改动"
  exit 0
fi
MANIFEST_LOCAL="$STAGE/manifest.json"
APPCAST_LOCAL="$STAGE/appcast.xml"

# 另一平台段可能已在线上：先取线上清单，--prev 合并后整体重签，避免覆盖另一平台段
if [ ! -s "$MANIFEST_LOCAL" ]; then
  curl -fsSL "$MANIFEST_PUBLIC" -o "$MANIFEST_LOCAL" 2>/dev/null || rm -f "$MANIFEST_LOCAL"
fi

# ---- ① 生成 manifest.json（v2，Ed25519 签名）----
MANIFEST_ARGS=(
  generate
  --product "$PRODUCT"
  --channel "$CHANNEL"
  --platform macos
  --version "$VERSION"
  --min-os "$MIN_OS"
  --artifact "dmg=$DMG_PATH"
  --key-id "$KEY_ID"
  --key "$SIGNING_KEY"
  --out "$MANIFEST_LOCAL"
)
[ -n "$MANDATORY" ] && [ "$MANDATORY" = "true" ] && \
  MANIFEST_ARGS+=(--mandatory --mandatory-reason MIN_SUPPORTED --mandatory-deadline "$PUBLISHED_AT")
[ -n "$RELEASE_NOTES_URL" ] && MANIFEST_ARGS+=(--notes-url "$RELEASE_NOTES_URL")
[ -s "$MANIFEST_LOCAL" ] && MANIFEST_ARGS+=(--prev "$MANIFEST_LOCAL")

echo ""
echo "===== ① 生成 manifest.json（v2）"
python3 "$MANIFEST_V2_PY" "${MANIFEST_ARGS[@]}"

# ---- ② 生成 appcast.xml（Sparkle 专用，不在 v2 范围）----
APPCAST_ARGS=(
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
  --out "$STAGE/latest.json"
  --appcast "$APPCAST_LOCAL"
  --published-at "$PUBLISHED_AT"
)
[ -n "$MANDATORY" ] && [ "$MANDATORY" = "true" ] && \
  APPCAST_ARGS+=(--mandatory --reason MIN_SUPPORTED --deadline "$PUBLISHED_AT")
[ -n "$RELEASE_NOTES_URL" ] && APPCAST_ARGS+=(--release-notes-url "$RELEASE_NOTES_URL")

echo ""
echo "===== ② 生成 appcast.xml（latest.json 仅为其副产物，不上传）"
"$VENV_PY" "$REPO_ROOT/tools/package/update_manifest.py" "${APPCAST_ARGS[@]}"

# ---- ③ 本地验签（上传前的双重保险）----
echo ""
echo "===== ③ 本地验签"
python3 "$MANIFEST_V2_PY" verify \
  --manifest "$MANIFEST_LOCAL" --pub "$SIGNING_PUB" --artifact "dmg=$DMG_PATH"

MANIFEST_KEY_ID="$($VENV_PY -c "import json;print(json.load(open('$MANIFEST_LOCAL'))['key_id'])")"
echo "  key_id=$MANIFEST_KEY_ID"

# ---- ④ 发布门禁（三仓库共用 tools/release/guard.py，v2 规则）----
GUARD_PY="${GUARD_PY:-$REPO_ROOT/tools/release/guard.py}"
GUARD_ARGS=(
  --product xiaotu --kind mac --channel "$CHANNEL"
  --file "$DMG_PATH" --name "$V2_DMG_NAME"
  --manifest "$MANIFEST_LOCAL" --pub "$SIGNING_PUB"
# 注：guard.py 已移除 --schema 参数（argparse 无此选项），强制 v2
)
[ "$OSS_SKIP" = "true" ] && GUARD_ARGS+=(--offline)
echo ""
echo "===== ④ 发布门禁"
python3 "$GUARD_PY" preflight "${GUARD_ARGS[@]}"

# ---- ⑤ 上传 dmg + manifest.json + appcast.xml ----
if [ "$OSS_SKIP" != "true" ]; then
  echo ""
  echo "===== ⑤ 上传"
  if [ -n "$RELEASE_NOTES_FILE" ]; then
    oss_upload "$RELEASE_NOTES_FILE" "$NOTES_KEY" \
      "public, max-age=3600" "text/html; charset=utf-8"
  fi
  oss_upload "$DMG_PATH" "$DMG_KEY" \
    "public, max-age=31536000, immutable" "application/octet-stream"
  oss_upload "$MANIFEST_LOCAL" "$MANIFEST_KEY" \
    "no-cache, max-age=60" "application/json"
  oss_upload "$APPCAST_LOCAL" "$APPCAST_KEY" \
    "no-cache, max-age=60" "application/rss+xml"
else
  echo ""
  echo "===== ⑤ [dry-run] 将上传 $DMG_KEY + $MANIFEST_KEY + $APPCAST_KEY"
  [ -n "$RELEASE_NOTES_FILE" ] && echo "===== ⑤ [dry-run] 将上传 $NOTES_KEY"
fi

# ---- ⑥ purge CDN ----
if [ "$OSS_SKIP" != "true" ]; then
  if [ "$PURGE" = "true" ]; then
    echo ""
    echo "===== ⑥ purge CDN 缓存"
    purge_urls=("$DMG_PUBLIC_URL" "$MANIFEST_PUBLIC" "$APPCAST_PUBLIC")
    [ -n "$RELEASE_NOTES_FILE" ] && purge_urls+=("$RELEASE_NOTES_URL")
    for url in "${purge_urls[@]}"; do
      if command -v aliyun >/dev/null 2>&1; then
        if aliyun cdn RefreshObjectCaches --ObjectPath "$url" >/dev/null 2>&1; then
          echo "  ✓ $url"
        else
          echo "  ✗ purge 失败：${url}（手动 purge）"
        fi
      else
        echo "  (手动 purge：$url)"
      fi
    done
    echo "  CDN purge 完成，1 分钟内全网生效"
  else
    echo ""
    echo "===== ⑥ 跳过 purge（PURGE=false）"
    echo "  提醒：新 manifest 最多 1 分钟后自动生效"
  fi
fi

# ---- ⑦ 预热 CDN ----
# 发布后主动下载一次新包，让边缘节点先缓存，避免首个用户吃冷启动（香港节点尤其明显）。
if [ "$OSS_SKIP" != "true" ] && [ "$WARMUP" = "true" ]; then
  echo ""
  echo "===== ⑦ 预热 CDN（下载一次新包写入边缘缓存）"
  if command -v curl >/dev/null 2>&1; then
    WARM_ERR="$(mktemp)"
    set +e
    WARM_RESULT="$(curl -sS -L -o /dev/null --max-time "$WARM_TIMEOUT" \
      -w '%{http_code} %{size_download} %{speed_download} %{time_total}' \
      "$DMG_PUBLIC_URL" 2>"$WARM_ERR")"
    WARM_RC=$?
    set -e
    read -r WARM_CODE WARM_BYTES WARM_SPEED WARM_TIME <<<"$WARM_RESULT"
    if [ "$WARM_RC" -eq 0 ] && [ "${WARM_CODE:-0}" = "200" ]; then
      WARM_MBPS="$(awk -v s="${WARM_SPEED:-0}" 'BEGIN{printf "%.2f", s/1048576}')"
      echo "  ✓ 已预热：HTTP $WARM_CODE · $WARM_BYTES 字节 · ${WARM_TIME}s · ${WARM_MBPS} MB/s"
    else
      echo "  ! 预热未完成（curl exit=${WARM_RC}，HTTP ${WARM_CODE:-?}）：$(tr '\n' ' ' <"$WARM_ERR")"
    fi
    rm -f "$WARM_ERR"
  else
    echo "  (未安装 curl，跳过预热)"
  fi
else
  echo ""
  echo "===== ⑦ [跳过] CDN 预热（WARMUP=false 或 dry-run）"
fi

if [ "$OSS_SKIP" != "true" ]; then
  echo ""
  echo "===== ⑧ 线上门禁"
  python3 "$GUARD_PY" verify-live "${GUARD_ARGS[@]}"
fi

# ---- ⑨ docs 自检：包体实测 + 实现状态巡检 ----
# 教训：设计文档会随时间漂移——包体数值偏离实际、实现状态写着「未实现」但代码早已完成。
# 这一节在发布脚本末尾打印两类提示，提醒发布者核对 docs/。
DOCS_ROOT="$REPO_ROOT/docs"
if [ -d "$DOCS_ROOT" ]; then
  echo ""
  echo "===== ⑨ docs 自检（包体实测 + 实现状态）"

  # 9.1 包体实测
  TECH_DOC="$DOCS_ROOT/进销存技术方案.md"
  if [ -f "$TECH_DOC" ]; then
    DMG_SIZE=$(stat -f%z "$DMG_PATH" 2>/dev/null || stat -c%s "$DMG_PATH" 2>/dev/null || echo "?")
    DMG_MIB=$(python3 -c "print(f'{int($DMG_SIZE)/1024/1024:.2f}')" 2>/dev/null || echo "?")
    TODAY=$(date '+%Y-%m-%d')
    echo "  📦 包体实测（版本 $VERSION / $TODAY）"
    echo "     DMG: ${DMG_SIZE}B (${DMG_MIB} MiB)"
    echo "     → 核对 docs/进销存技术方案.md §2.6「当前实测」表是否需要追加新行"
  fi

  # 9.2 实现状态巡检：扫描 docs/ 里出现的「未实现 / 待补 / 缺口 / TODO」字样
  # 这些是技术方案文档里最容易漂移的部分——代码写完了但文档没跟进。
  WATCH_WORDS="未实现|待补|待处理|TODO|FIXME|缺口|缺失|尚未|计划实现"
  HITS=$(grep -rnE "$WATCH_WORDS" "$DOCS_ROOT" \
    --include="*.md" \
    -l 2>/dev/null || true)
  if [ -n "$HITS" ]; then
    echo ""
    echo "  🔍 实现状态巡检（docs/ 中含「未实现/待补/TODO」等字样）"
    while IFS= read -r f; do
      COUNT=$(grep -cE "$WATCH_WORDS" "$f" 2>/dev/null || echo 0)
      REL="${f#$REPO_ROOT/}"
      echo "     $REL ($COUNT 处)"
      grep -nE "$WATCH_WORDS" "$f" 2>/dev/null | head -3 | sed 's/^/       L/'
    done <<< "$HITS"
    echo ""
    echo "     → 发布前核对这些条目的代码现状；已实现的及时改掉，"
    echo "       避免下次再踩「代码写完了文档还说未实现」的坑。"
  else
    echo ""
    echo "  🔍 实现状态巡检：docs/ 未发现待补标记 ✓"
  fi
fi

# ---- ⑩ 完成 ----
echo ""
echo "================ 发布完成 ================"
echo "版本 / build / channel：  $VERSION / $BUILD / $CHANNEL"
echo "dmg 下载地址：           $DMG_PUBLIC_URL"
echo "manifest.json：          $MANIFEST_PUBLIC"
echo "appcast.xml：            $APPCAST_PUBLIC"
echo "key_id：                 $MANIFEST_KEY_ID"
echo ""
if [ "${OSS_CRED_SOURCE:-}" = "环境变量" ]; then
  echo "→ 提醒：本次凭据来自环境变量。若曾明文粘贴到聊天、命令行历史或日志，"
  echo "  请到 RAM 控制台轮换这对密钥，并改用 ${OSS_CRED_FILE}（chmod 600）。"
  echo ""
fi
echo "→ 客户端 Info.plist 注入（make_dmg.sh 参数）："
echo "  XIAOTU_UPDATE_MANIFEST_URL=$MANIFEST_PUBLIC"
echo "  XIAOTU_UPDATE_FEED_URL=$APPCAST_PUBLIC"
echo "  XIAOTU_UPDATE_PUBKEY=$PUB_BASE64"
echo ""
echo "→ 立即验证："
if [ "$OSS_SKIP" = "true" ]; then
  echo "  python3 tools/release/manifest_v2.py verify --manifest $MANIFEST_LOCAL --pub $SIGNING_PUB --artifact dmg=$DMG_PATH"
else
  echo "  curl -s $MANIFEST_PUBLIC | python3 -m json.tool | head -15"
  echo "  python3 tools/release/guard.py verify-live ${GUARD_ARGS[*]}"
fi
