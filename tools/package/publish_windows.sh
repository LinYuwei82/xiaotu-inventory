#!/usr/bin/env bash
# 小兔在线 · Windows 清单发布脚本（更新清单 Schema v2）
#
# 输入 Windows 安装包（setup.exe）+ 可选绿色包 → 生成 Ed25519 签名的 manifest.json（v2）
# → 上传 dl.xiaotu.online → purge CDN。
# macOS 用 publish_oss.sh（dmg + appcast）；Windows 没有 appcast，只发清单。
#
# 目录结构（与壳里写死的清单地址一致）：
#   oss://<bucket>/xiaotu/xiaotu-<ver>-win64-setup.exe            ← 长缓存 / immutable（v2 命名）
#   oss://<bucket>/xiaotu/xiaotu-<ver>-win64-portable.zip         ← 存在时一并上传
#   oss://<bucket>/xiaotu/updates/<channel>/manifest.json         ← 60 秒缓存（v2 清单）
#
# 本地产物名仍为 Windows 构建输出（xiaotu-inventory-*），上传时按 v2 规则改名。
# 本脚本不再生成 windows.json，旧内测客户端按切换方案重装升级。
#
# 重要：BUILD 必须与打包时写进 exe 的构建号完全一致。
# 壳的更新判定是 `manifest.build <= 当前 build → 无更新`；如果清单里的 build
# 与安装包内的 build 不一致，客户端会反复提示更新。打包机在 Windows 上执行
#   cmake --preset windows-release -DXIAOTU_RELEASE_BUILD=ON -DXIAOTU_BUILD=<BUILD>
#   set XIAOTU_BUILD=<BUILD> & cmake -S shell\win -B shell\win\build ...
# 然后从 build.ninja 的 DEFINES 里确认两处都是同一个 <BUILD> 再发布。
#
# 凭据与签名私钥：与 publish_oss.sh 相同（私钥只在 Mac 上）：
#   ~/.config/xiaotu-updates/update_signing_2026.pem
#   ~/.config/xiaotu-updates/oss-credentials.env（600 权限）
#
# 用法：
#   BUILD=202611171349 OSS_BUCKET=xxx OSS_REGION=cn-hangzhou PRODUCT=xiaotu tools/package/publish_windows.sh
#
#   # 只本地生成 + 验签，不碰 OSS
#   OSS_SKIP=true BUILD=202611171349 tools/package/publish_windows.sh
#
#   # 安装包不在默认位置时显式指定
#   SETUP_PATH=/tmp/xiaotu-inventory-1.0.0-win64-setup.exe BUILD=... tools/package/publish_windows.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

VERSION="$(tr -d '[:space:]' < "$REPO_ROOT/VERSION" 2>/dev/null)"
[ -n "$VERSION" ] || { echo "无法从仓库根 VERSION 文件读取版本号。" >&2; exit 1; }

usage() {
  sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}
case "${1:-}" in -h|--help) usage; exit 0 ;; "") ;;
  *) echo "未知参数：$1（用 --help 查看用法）" >&2; exit 2 ;; esac

# ---- 核心参数 ----
PRODUCT="${PRODUCT:-xiaotu}"
case "$PRODUCT" in
  xiaotu) ;;
  devcard|mote)
    echo "PRODUCT=$PRODUCT 已迁出本仓库：DevCard 用 dev_card_cpp/tools/release/publish_windows.ps1，Mote 用 mote/tools/release/publish_windows.sh" >&2
    exit 1 ;;
  *) echo "PRODUCT 必须是 xiaotu" >&2; exit 1 ;;
esac

OSS_BUCKET="${OSS_BUCKET:-}"
OSS_REGION="${OSS_REGION:-}"
CDN_DOMAIN="${CDN_DOMAIN:-https://dl.xiaotu.online}"
OSS_SKIP="${OSS_SKIP:-false}"

# ---- OSS 凭据文件：环境变量优先，其次仓库外 600 权限的凭据文件 ----
OSS_CRED_FILE="${OSS_CRED_FILE:-$HOME/.config/xiaotu-updates/oss-credentials.env}"
# 兼容只有 .env 的环境（内容为 export KEY=VALUE）
if [ ! -f "$OSS_CRED_FILE" ] && [ -f "$HOME/.config/xiaotu-updates/.env" ]; then
  OSS_CRED_FILE="$HOME/.config/xiaotu-updates/.env"
fi

cred_value() {  # cred_value <文件> <键名>
  sed -n "s/^[[:space:]]*\(export[[:space:]]\{1,\}\)\{0,1\}$2[[:space:]]*=[[:space:]]*//p" "$1" | tail -1 \
    | tr -d '\r' | sed -e 's/^"//' -e 's/"$//' -e "s/^'//" -e "s/'$//"
}

# 桶名 / 区域也允许写在凭据文件里，省去每次手工 export
if [ -z "$OSS_BUCKET" ] && [ -f "$OSS_CRED_FILE" ]; then
  OSS_BUCKET="$(cred_value "$OSS_CRED_FILE" OSS_BUCKET)"
fi
if [ -z "$OSS_REGION" ] && [ -f "$OSS_CRED_FILE" ]; then
  OSS_REGION="$(cred_value "$OSS_CRED_FILE" OSS_REGION)"
fi

if [ "$OSS_SKIP" != "true" ]; then
  if [ -z "$OSS_BUCKET" ] || [ -z "$OSS_REGION" ]; then
    echo "缺少必填：OSS_BUCKET / OSS_REGION" >&2
    echo "需要 OSS_ACCESS_KEY_ID / OSS_ACCESS_KEY_SECRET 环境变量" >&2
    echo "如需只跑本地生成验证：OSS_SKIP=true tools/package/publish_windows.sh" >&2
    exit 1
  fi
  case "$CDN_DOMAIN" in https://*|http://*) ;;
    *) echo "CDN_DOMAIN 必须以 http(s):// 开头" >&2; exit 1 ;; esac
else
  echo "[dry-run] OSS_SKIP=true：跳过 OSS 上传与 purge"
fi

CHANNEL="${CHANNEL:-stable}"
case "$CHANNEL" in stable|beta) ;; *) echo "CHANNEL 必须是 stable/beta" >&2; exit 1 ;; esac

BUILD="${BUILD:-}"
[[ "$BUILD" =~ ^[0-9]{4,}$ ]] || {
  echo "缺少或非法的 BUILD：需要与打包时 XIAOTU_BUILD 相同的纯数字构建号（例：BUILD=202611171349）" >&2
  exit 1
}

DEFAULT_SETUP="$REPO_ROOT/packaging/win/out/xiaotu-inventory-$VERSION-win64-setup.exe"
SETUP_PATH="${SETUP_PATH:-$DEFAULT_SETUP}"
[ -f "$SETUP_PATH" ] || { echo "安装包不存在：$SETUP_PATH" >&2; exit 1; }

# 绿色包（zip）由 Windows 构建产出；存在就一并发布并写进清单，
# 下载页的绿色包行才有数据可同步（没有时清单只描述安装包）。
DEFAULT_ZIP="$REPO_ROOT/build/package/win/xiaotu-inventory-$VERSION-win64.zip"
ZIP_PATH="${ZIP_PATH:-$DEFAULT_ZIP}"
ZIP_KEY=""
ZIP_PUBLIC_URL=""
ZIP_SIZE=""
ZIP_SHA256=""
if [ -f "$ZIP_PATH" ]; then
  ZIP_SIZE="$(stat -f %z "$ZIP_PATH" 2>/dev/null || stat -c %s "$ZIP_PATH")"
  ZIP_SHA256="$(shasum -a 256 "$ZIP_PATH" | awk '{print $1}')"
fi

# 清单里的下载地址由 v2 命名规则推导（本地文件名可以保持构建输出不变，远端按 v2 改名）。
EXPECTED_NAME="xiaotu-inventory-$VERSION-win64-setup.exe"
ACTUAL_NAME="$(basename "$SETUP_PATH")"
if [ "$ACTUAL_NAME" != "$EXPECTED_NAME" ]; then
  echo "安装包文件名不符合构建输出规范：$ACTUAL_NAME" >&2
  echo "必须命名为 ${EXPECTED_NAME}（版本号从它推导）。" >&2
  echo "可先改名或放到 packaging/win/out/$EXPECTED_NAME 再重试。" >&2
  exit 1
fi

# v2 安装包命名：<product>-<version>-<os>-<arch>-<type>.<ext>（见更新清单Schema-v2.md 第七节）
V2_SETUP_NAME="xiaotu-${VERSION}-win64-setup.exe"
V2_ZIP_NAME="xiaotu-${VERSION}-win64-portable.zip"

DEFAULT_KEY="$HOME/.config/xiaotu-updates/update_signing_2026.pem"
SIGNING_KEY="${SIGNING_KEY:-$DEFAULT_KEY}"
[ -f "$SIGNING_KEY" ] || {
  echo "签名私钥不存在：$SIGNING_KEY" >&2
  echo "私钥只在打包/签名机上；Windows 发布请在 Mac 上执行本脚本。" >&2; exit 1; }
SIGNING_PUB="${SIGNING_KEY%.pem}.pub.pem"
[ -f "$SIGNING_PUB" ] || { echo "公钥不存在：$SIGNING_PUB" >&2; exit 1; }

KEY_ID="${KEY_ID:-2026-11}"
MIN_OS="${MIN_OS:-10.0}"
RELEASE_NOTES_URL="${RELEASE_NOTES_URL:-}"
PURGE="${PURGE:-true}"

# ---- 依赖：manifest_v2.py / guard.py 均为纯标准库，python3 即可 ----
command -v python3 >/dev/null 2>&1 || { echo "未找到 python3" >&2; exit 1; }
command -v shasum >/dev/null 2>&1 || { echo "未找到 shasum" >&2; exit 1; }

# ---- 凭据文件路径与解析函数已在脚本开头定义 ----
mask_ak() {
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

assert_cred_not_tracked() {
  git -C "$REPO_ROOT" rev-parse --git-dir >/dev/null 2>&1 || return 0
  if git -C "$REPO_ROOT" grep -qF -- "$OSS_ACCESS_KEY_ID" -- . 2>/dev/null; then
    echo "✗ AccessKey 出现在仓库受版本控制的文件里，已拒绝发布。" >&2
    exit 1
  fi
}

if [ "$OSS_SKIP" != "true" ]; then
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

# ---- 路径 ----
SETUP_SIZE="$(stat -f %z "$SETUP_PATH" 2>/dev/null || stat -c %s "$SETUP_PATH")"
SETUP_SHA256="$(shasum -a 256 "$SETUP_PATH" | awk '{print $1}')"
PUBLISHED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

SETUP_KEY="$PRODUCT_DIR/$V2_SETUP_NAME"
SETUP_PUBLIC_URL="$CDN_DOMAIN/$SETUP_KEY"
[ -n "$ZIP_SHA256" ] && ZIP_KEY="$PRODUCT_DIR/$V2_ZIP_NAME" && ZIP_PUBLIC_URL="$CDN_DOMAIN/$ZIP_KEY"
MANIFEST_KEY="$PRODUCT_DIR/updates/$CHANNEL/manifest.json"
MANIFEST_PUBLIC="$CDN_DOMAIN/$MANIFEST_KEY"

echo "===== OSS 发布 · Windows 安装包 ====="
echo "版本 / build / channel：  ${VERSION} / ${BUILD} / ${CHANNEL}"
echo "安装包：                  ${SETUP_PATH} （${SETUP_SIZE} 字节）"
echo "远端文件名（v2）：        ${V2_SETUP_NAME}"
echo "sha256：                  ${SETUP_SHA256}"
echo "bucket / region：         ${OSS_BUCKET:-<dry-run>} / ${OSS_REGION:-<dry-run>}"
echo "AK / 来源：               ${AK_MASK:-<dry-run>} / ${OSS_CRED_SOURCE:-<dry-run>}"
echo "域名：                    ${CDN_DOMAIN}"
echo "OSS key："
echo "  setup.exe：             ${SETUP_KEY}"
[ -n "$ZIP_KEY" ] && echo "  zip（绿色包）：         ${ZIP_KEY}（${ZIP_SIZE} 字节）"
echo "  manifest.json：         ${MANIFEST_KEY}"
[ "${OSS_SKIP}" = "true" ] && echo "(dry-run：不上传)"

STAGE="$(mktemp -d -t xiaotu-win-stage)"
trap 'rm -rf "$STAGE"' EXIT

oss_upload() {
  # oss_upload <local_path> <oss_key> <cache_control> <content_type>
  local LP="$1" OK="$2" CC="$3" CT="$4"
  "$OSS_PY" - "$OSS_BUCKET" "$OSS_REGION" "$OSS_ACCESS_KEY_ID" "$OSS_ACCESS_KEY_SECRET" \
    "$LP" "$OK" "$CC" "$CT" <<'PY_EOF'
import sys, oss2
bucket, region, ak, sk, lp, ok, cc, ct = sys.argv[1:]
endpoint = f"https://oss-{region}.aliyuncs.com"
bkt = oss2.Bucket(oss2.Auth(ak, sk), endpoint, bucket)
with open(lp, "rb") as f:
    bkt.put_object(ok, f, headers={"Cache-Control": cc, "Content-Type": ct})
print(f"  ✓ {ok}  ({ct}, {cc})")
PY_EOF
}

MANIFEST_V2_PY="$REPO_ROOT/tools/release/manifest_v2.py"
MANIFEST_LOCAL="$STAGE/manifest.json"

# ---- ① 生成并签名 manifest.json（v2）----
MANIFEST_ARGS=(
  generate
  --product "$PRODUCT"
  --channel "$CHANNEL"
  --platform windows
  --version "$VERSION"
  --build "$BUILD"
  --min-os "$MIN_OS"
  --artifact "setup=$SETUP_PATH"
  --key-id "$KEY_ID"
  --key "$SIGNING_KEY"
  --out "$MANIFEST_LOCAL"
)
[ -f "$ZIP_PATH" ] && MANIFEST_ARGS+=(--artifact "portable=$ZIP_PATH")
[ -n "$RELEASE_NOTES_URL" ] && MANIFEST_ARGS+=(--notes-url "$RELEASE_NOTES_URL")

echo ""
echo "===== ① 生成 manifest.json（v2）"
python3 "$MANIFEST_V2_PY" "${MANIFEST_ARGS[@]}"

# ---- ② 本地验签 ----
echo ""
echo "===== ② 本地验签"
VERIFY_ARGS=(--manifest "$MANIFEST_LOCAL" --pub "$SIGNING_PUB" --artifact "setup=$SETUP_PATH")
[ -f "$ZIP_PATH" ] && VERIFY_ARGS+=(--artifact "portable=$ZIP_PATH")
python3 "$MANIFEST_V2_PY" verify "${VERIFY_ARGS[@]}"

# ---- ③ 发布门禁（三仓库共用 tools/release/guard.py，v2 规则）----
# 校验 v2 命名、版本/构建号单调、同名覆盖风险、manifest.json 签名与产物一致性，
# 以及绿色包内壳的清单地址（v2 要求指向 manifest.json）。
GUARD_PY="${GUARD_PY:-$REPO_ROOT/tools/release/guard.py}"
GUARD_ARGS=(
  --product xiaotu --kind win --channel "$CHANNEL"
  --file "$SETUP_PATH" --name "$V2_SETUP_NAME"
  --schema v2 --manifest "$MANIFEST_LOCAL" --pub "$SIGNING_PUB" --build "$BUILD"
)
if [ -f "$ZIP_PATH" ]; then
  GUARD_ARGS+=(--zip "$ZIP_PATH" --zip-name "$V2_ZIP_NAME")
fi
[ "$OSS_SKIP" = "true" ] && GUARD_ARGS+=(--offline)
echo ""
echo "===== ③ 发布门禁"
python3 "$GUARD_PY" preflight "${GUARD_ARGS[@]}"

# ---- ④ 上传 setup.exe / 绿色包 / manifest.json ----
if [ "$OSS_SKIP" != "true" ]; then
  echo ""
  echo "===== ④ 上传安装包 → $SETUP_KEY"
  oss_upload "$SETUP_PATH" "$SETUP_KEY" \
    "public, max-age=31536000, immutable" "application/octet-stream"
  if [ -n "$ZIP_KEY" ]; then
    echo "===== ④ 上传绿色包 → $ZIP_KEY"
    oss_upload "$ZIP_PATH" "$ZIP_KEY" \
      "public, max-age=31536000, immutable" "application/zip"
  fi
  echo "===== ④ 上传 manifest.json → $MANIFEST_KEY"
  oss_upload "$MANIFEST_LOCAL" "$MANIFEST_KEY" \
    "no-cache, max-age=60" "application/json"
  echo ""
  echo "===== ④b 线上门禁"
  python3 "$GUARD_PY" verify-live "${GUARD_ARGS[@]}"
else
  echo ""
  echo "===== ④ [dry-run] 将上传 $SETUP_KEY + $MANIFEST_KEY"
  [ -n "$ZIP_KEY" ] && echo "===== ④ [dry-run] 将上传 $ZIP_KEY"
fi

# ---- ⑤ purge CDN ----
if [ "$OSS_SKIP" != "true" ] && [ "$PURGE" = "true" ]; then
  echo ""
  echo "===== ⑤ purge CDN 缓存"
  purge_urls=("$SETUP_PUBLIC_URL" "$MANIFEST_PUBLIC")
  [ -n "$ZIP_KEY" ] && purge_urls+=("$ZIP_PUBLIC_URL")
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
elif [ "$OSS_SKIP" != "true" ]; then
  echo ""
  echo "===== ⑤ 跳过 purge（PURGE=false）"
fi

# ---- 完成 ----
echo ""
echo "================ 发布完成 ================"
echo "版本 / build / channel：  $VERSION / $BUILD / $CHANNEL"
echo "安装包下载地址：          $SETUP_PUBLIC_URL"
echo "  sha256 / size：         $SETUP_SHA256 / $SETUP_SIZE 字节"
[ -n "$ZIP_KEY" ] && echo "绿色包下载地址：          $ZIP_PUBLIC_URL
  sha256 / size：         $ZIP_SHA256 / $ZIP_SIZE 字节"
echo "manifest.json：            $MANIFEST_PUBLIC"
echo ""
echo "→ 立即验证："
if [ "$OSS_SKIP" = "true" ]; then
  echo "  python3 tools/release/manifest_v2.py verify --manifest $MANIFEST_LOCAL --pub $SIGNING_PUB --artifact setup=$SETUP_PATH"
else
  echo "  curl -s $MANIFEST_PUBLIC | python3 -m json.tool | head -15"
  echo "  python3 tools/release/guard.py verify-live ${GUARD_ARGS[*]}"
fi
