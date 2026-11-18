#!/usr/bin/env bash
# 小兔在线 · Windows 清单发布脚本
#
# 输入 Windows 安装包（setup.exe）→ 生成 Ed25519 签名的 windows.json → 上传 dl.xiaotu.online → purge CDN。
# macOS 用 publish_oss.sh（dmg + appcast）；Windows 没有 appcast，只发清单。
#
# 目录结构（与壳里写死的清单地址一致）：
#   oss://<bucket>/xiaotu/<setup 文件名>                 ← 长缓存 / immutable（下载页也用这个路径）
#   oss://<bucket>/xiaotu/<绿色包文件名>                 ← 存在时一并上传；清单写明 portable_*
#   oss://<bucket>/<product>/updates/<channel>/windows.json ← 60 秒缓存
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
  ZIP_BASENAME="$(basename "$ZIP_PATH")"
  ZIP_KEY="$PRODUCT/$ZIP_BASENAME"
  ZIP_PUBLIC_URL="$CDN_DOMAIN/$ZIP_KEY"
  ZIP_SIZE="$(stat -f %z "$ZIP_PATH" 2>/dev/null || stat -c %s "$ZIP_PATH")"
  ZIP_SHA256="$(shasum -a 256 "$ZIP_PATH" | awk '{print $1}')"
fi

# 清单里的下载地址直接由文件名推导，文件名不规范会写出指向错误对象的 URL
# （例如从浏览器另存为 win_setup.exe，清单就指向 /xiaotu/win_setup.exe）。
EXPECTED_NAME="xiaotu-inventory-$VERSION-win64-setup.exe"
ACTUAL_NAME="$(basename "$SETUP_PATH")"
if [ "$ACTUAL_NAME" != "$EXPECTED_NAME" ]; then
  echo "安装包文件名不符合发布规范：$ACTUAL_NAME" >&2
  echo "必须命名为 ${EXPECTED_NAME}（清单下载地址由它推导）。" >&2
  echo "可先改名或放到 packaging/win/out/$EXPECTED_NAME 再重试。" >&2
  exit 1
fi

DEFAULT_KEY="$HOME/.config/xiaotu-updates/update_signing_2026.pem"
SIGNING_KEY="${SIGNING_KEY:-$DEFAULT_KEY}"
[ -f "$SIGNING_KEY" ] || {
  echo "签名私钥不存在：$SIGNING_KEY" >&2
  echo "私钥只在打包/签名机上；Windows 发布请在 Mac 上执行本脚本。" >&2; exit 1; }
SIGNING_PUB="${SIGNING_KEY%.pem}.pub.pem"
[ -f "$SIGNING_PUB" ] || { echo "公钥不存在：$SIGNING_PUB" >&2; exit 1; }

KEY_ID="${KEY_ID:-2026-11}"
SCHEMA_MIN="${SCHEMA_MIN:-1}"
SCHEMA_MAX="${SCHEMA_MAX:-39}"
API_MIN="${API_MIN:-1}"
API_MAX="${API_MAX:-1}"
RELEASE_NOTES_URL="${RELEASE_NOTES_URL:-}"
PURGE="${PURGE:-true}"

# ---- 依赖：venv + cryptography ----
VENV_PY="$REPO_ROOT/tools/package/.venv/bin/python"
if [ ! -x "$VENV_PY" ]; then
  python3 -m venv "$REPO_ROOT/tools/package/.venv"
  "$REPO_ROOT/tools/package/.venv/bin/pip" install -q -r "$REPO_ROOT/tools/package/requirements.txt"
fi
command -v shasum >/dev/null 2>&1 || { echo "未找到 shasum" >&2; exit 1; }

# ---- ⓪ 发布门禁（三仓库共用 tools/release/guard.py）----
# 校验命名、版本单调、同名覆盖风险，以及绿色包内壳的清单地址。
GUARD_PY="${GUARD_PY:-$REPO_ROOT/tools/release/guard.py}"
GUARD_ARGS=(--product xiaotu --kind win --channel "$CHANNEL")
if [ -n "$ZIP_KEY" ]; then
  GUARD_ARGS+=(--file "$SETUP_PATH" --zip "$ZIP_PATH")
else
  GUARD_ARGS+=(--file "$SETUP_PATH")
fi
[ "$OSS_SKIP" = "true" ] && GUARD_ARGS+=(--offline)
echo ""
echo "===== ⓪ 发布门禁"
python3 "$GUARD_PY" preflight "${GUARD_ARGS[@]}"

# ---- OSS 凭据：环境变量优先，其次仓库外 600 权限的凭据文件 ----
OSS_CRED_FILE="${OSS_CRED_FILE:-$HOME/.config/xiaotu-updates/oss-credentials.env}"
# 兼容只有 .env 的环境（内容为 export KEY=VALUE）
if [ ! -f "$OSS_CRED_FILE" ] && [ -f "$HOME/.config/xiaotu-updates/.env" ]; then
  OSS_CRED_FILE="$HOME/.config/xiaotu-updates/.env"
fi

cred_value() {  # cred_value <文件> <键名>
  sed -n "s/^[[:space:]]*\(export[[:space:]]\{1,\}\)\{0,1\}$2[[:space:]]*=[[:space:]]*//p" "$1" | tail -1 \
    | tr -d '\r' | sed -e 's/^"//' -e 's/"$//' -e "s/^'//" -e "s/'$//"
}

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
SETUP_BASENAME="$(basename "$SETUP_PATH")"
SETUP_SIZE="$(stat -f %z "$SETUP_PATH" 2>/dev/null || stat -c %s "$SETUP_PATH")"
SETUP_SHA256="$(shasum -a 256 "$SETUP_PATH" | awk '{print $1}')"
PUBLISHED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

SETUP_KEY="$PRODUCT/$SETUP_BASENAME"
SETUP_PUBLIC_URL="$CDN_DOMAIN/$SETUP_KEY"
LATEST_KEY="$PRODUCT/updates/$CHANNEL/windows.json"
MANIFEST_PUBLIC="$CDN_DOMAIN/$LATEST_KEY"

echo "===== OSS 发布 · Windows 安装包 ====="
echo "版本 / build / channel：  ${VERSION} / ${BUILD} / ${CHANNEL}"
echo "安装包：                  ${SETUP_PATH} （${SETUP_SIZE} 字节）"
echo "sha256：                  ${SETUP_SHA256}"
echo "bucket / region：         ${OSS_BUCKET:-<dry-run>} / ${OSS_REGION:-<dry-run>}"
echo "AK / 来源：               ${AK_MASK:-<dry-run>} / ${OSS_CRED_SOURCE:-<dry-run>}"
echo "域名：                    ${CDN_DOMAIN}"
echo "OSS key："
echo "  setup.exe：             ${SETUP_KEY}"
[ -n "$ZIP_KEY" ] && echo "  zip（绿色包）：         ${ZIP_KEY}（${ZIP_SIZE} 字节）"
echo "  windows.json：          ${LATEST_KEY}"
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

# ---- ① 上传 setup.exe ----
if [ "$OSS_SKIP" != "true" ]; then
  echo ""
  echo "===== ① 上传安装包 → $SETUP_KEY"
  oss_upload "$SETUP_PATH" "$SETUP_KEY" \
    "public, max-age=31536000, immutable" "application/octet-stream"
  if [ -n "$ZIP_KEY" ]; then
    echo "===== ① 上传绿色包 → $ZIP_KEY"
    oss_upload "$ZIP_PATH" "$ZIP_KEY" \
      "public, max-age=31536000, immutable" "application/zip"
  fi
else
  echo ""
  echo "===== ① [dry-run] 将上传 $SETUP_KEY"
  [ -n "$ZIP_KEY" ] && echo "===== ① [dry-run] 将上传 $ZIP_KEY"
fi

# ---- ② 生成并签名 windows.json ----
MANIFEST_LOCAL="$STAGE/windows.json"
MANIFEST_ARGS=(
  generate
  --file "$SETUP_PATH"
  --version "$VERSION"
  --build "$BUILD"
  --url "$SETUP_PUBLIC_URL"
  --platform windows
  --channel "$CHANNEL"
  --schema-min "$SCHEMA_MIN"
  --schema-max "$SCHEMA_MAX"
  --api-min "$API_MIN"
  --api-max "$API_MAX"
  --rollout 100
  --key-id "$KEY_ID"
  --key "$SIGNING_KEY"
  --out "$MANIFEST_LOCAL"
  --published-at "$PUBLISHED_AT"
)
[ -n "$RELEASE_NOTES_URL" ] && MANIFEST_ARGS+=(--release-notes-url "$RELEASE_NOTES_URL")
[ -n "$ZIP_KEY" ] && MANIFEST_ARGS+=(--portable-file "$ZIP_PATH" --portable-url "$ZIP_PUBLIC_URL")

echo ""
echo "===== ② 生成 windows.json"
"$VENV_PY" "$REPO_ROOT/tools/package/update_manifest.py" "${MANIFEST_ARGS[@]}"

# ---- ③ 本地验签 ----
echo ""
echo "===== ③ 本地验签"
"$VENV_PY" "$REPO_ROOT/tools/package/update_manifest.py" verify \
  --manifest "$MANIFEST_LOCAL" --pub "$SIGNING_PUB" --file "$SETUP_PATH"

MANIFEST_BUILD="$("$VENV_PY" -c "import json;print(json.load(open('$MANIFEST_LOCAL'))['build'])")"
[ "$MANIFEST_BUILD" = "$BUILD" ] || {
  echo "清单 build（${MANIFEST_BUILD}）与 BUILD（${BUILD}）不一致" >&2; exit 1; }

# ---- ④ 上传 windows.json ----
if [ "$OSS_SKIP" != "true" ]; then
  echo ""
  echo "===== ④ 上传 windows.json"
  oss_upload "$MANIFEST_LOCAL" "$LATEST_KEY" \
    "no-cache, max-age=60" "application/json"
  echo ""
  echo "===== ④b 线上门禁"
  python3 "$GUARD_PY" verify-live "${GUARD_ARGS[@]}"
else
  echo ""
  echo "===== ④ [dry-run] 将上传 $LATEST_KEY"
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
echo "windows.json：             $MANIFEST_PUBLIC"
echo "schema / api：            $SCHEMA_MIN ~ $SCHEMA_MAX / $API_MIN ~ $API_MAX"
echo ""
echo "→ 立即验证："
if [ "$OSS_SKIP" = "true" ]; then
  echo "  python3 tools/package/update_manifest.py verify --manifest $MANIFEST_LOCAL --pub $SIGNING_PUB --file $SETUP_PATH"
else
  echo "  curl -s $MANIFEST_PUBLIC | python3 -m json.tool | head -15"
  echo "  python3 tools/package/update_manifest.py verify --manifest <(curl -s $MANIFEST_PUBLIC) --pub $SIGNING_PUB --file $SETUP_PATH"
fi
