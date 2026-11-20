#!/usr/bin/env bash
# 小兔在线 · 自动逐档放量脚本
#
# 对一份已签名的 manifest.json（Schema v2）按计划依次重签
# platforms.<platform>.rollout_percent、上传并 purge CDN，每档停留指定观察时长后
# 自动进入下一档。清单其余内容（版本、build、URL、sha256、强制字段）全部保留；
# appcast.xml 不随比例变化，脚本不重传。
#
# 默认节奏：1% → 10% → 50% → 100%，每档 24 小时。
# 放量只改清单；单机版壳与网络版服务端都按安装 ID 稳定分桶，已在 100% 的旧清单
# 上重签，不会影响已升级用户。
#
# 用法：
#   # 先用发布脚本产出清单（dist/manifest.json），或从线上取回
#   curl -fsS https://dl.xiaotu.online/xiaotu/updates/stable/manifest.json \
#     -o dist/manifest.json
#
#   # 预览（只本地重签，不上传、不等待）
#   tools/package/rollout_oss.sh --manifest dist/manifest.json --dry-run
#
#   # 正式放量（会要求输入 yes；每档之间自动等待）
#   OSS_BUCKET=xxx OSS_REGION=cn-hangzhou \
#     tools/package/rollout_oss.sh --manifest dist/manifest.json --yes
#
#   # 只放量 Windows（默认 all：同时改 macOS 与 Windows 两段）
#   tools/package/rollout_oss.sh --platform windows --yes
#
#   # 自定义节奏
#   tools/package/rollout_oss.sh --steps 5,20,60,100 --interval 12h --yes
#
# 参数：
#   --manifest PATH     已签名 v2 清单（默认 dist/manifest.json）
#   --platform NAME     all / macos / windows（默认 all）
#   --steps LIST        逗号分隔、递增且以 100 结尾（默认 1,10,50,100）
#   --interval DUR      每档停留时长，支持 s/m/h/d（默认 24h）
#   --product NAME      仅支持 xiaotu（默认 xiaotu；DevCard / Mote 已迁出本仓库）
#   --channel NAME      stable / beta（默认 stable，且必须与清单一致）
#   --key PATH          Ed25519 私钥（默认 ~/.config/xiaotu-updates/update_signing_2026.pem）
#   --poll-timeout SEC  上传后等待 CDN 生效的秒数（默认 180）
#   --dry-run           只重签并打印计划，不上传、不等待
#   --yes               跳过开跑确认
#
# 凭据：同 publish_macos.sh，优先 OSS_ACCESS_KEY_ID / OSS_ACCESS_KEY_SECRET，
#       否则读 ~/.config/xiaotu-updates/oss-credentials.env（权限 600/400）。
# 中止：Ctrl-C 随时停止，当前档位保持生效；不会回退比例。
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

usage() {
  sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

MANIFEST="${MANIFEST:-$REPO_ROOT/dist/manifest.json}"
PLATFORM="${PLATFORM:-all}"
STEPS="1,10,50,100"
INTERVAL="24h"
PRODUCT="${PRODUCT:-xiaotu}"
CHANNEL="${CHANNEL:-stable}"
POLL_TIMEOUT="${POLL_TIMEOUT:-180}"
CDN_DOMAIN="${CDN_DOMAIN:-https://dl.xiaotu.online}"
DRY_RUN="false"
ASSUME_YES="false"
DEFAULT_KEY="$HOME/.config/xiaotu-updates/update_signing_2026.pem"
SIGNING_KEY="${SIGNING_KEY:-$DEFAULT_KEY}"
OSS_CRED_FILE="${OSS_CRED_FILE:-$HOME/.config/xiaotu-updates/oss-credentials.env}"
# 兼容只有 .env 的环境（内容为 export KEY=VALUE）
if [ ! -f "$OSS_CRED_FILE" ] && [ -f "$HOME/.config/xiaotu-updates/.env" ]; then
  OSS_CRED_FILE="$HOME/.config/xiaotu-updates/.env"
fi

while [ $# -gt 0 ]; do
  case "$1" in
    --manifest) MANIFEST="${2:?--manifest 需要路径}"; shift 2 ;;
    --platform) PLATFORM="${2:?--platform 需要名称}"; shift 2 ;;
    --steps) STEPS="${2:?--steps 需要列表}"; shift 2 ;;
    --interval) INTERVAL="${2:?--interval 需要时长}"; shift 2 ;;
    --product) PRODUCT="${2:?--product 需要名称}"; shift 2 ;;
    --channel) CHANNEL="${2:?--channel 需要名称}"; shift 2 ;;
    --key) SIGNING_KEY="${2:?--key 需要路径}"; shift 2 ;;
    --poll-timeout) POLL_TIMEOUT="${2:?--poll-timeout 需要秒数}"; shift 2 ;;
    --dry-run) DRY_RUN="true"; shift ;;
    --yes|-y) ASSUME_YES="true"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "未知参数：$1（用 --help 查看用法）" >&2; exit 2 ;;
  esac
done

case "$PRODUCT" in
  xiaotu) ;;
  devcard|mote)
    echo "PRODUCT=$PRODUCT 已迁出本仓库：请到对应仓库执行放量" >&2
    exit 1 ;;
  *) echo "PRODUCT 必须是 xiaotu" >&2; exit 1 ;;
esac

case "$CHANNEL" in
  stable|beta) ;;
  *) echo "CHANNEL 必须是 stable / beta" >&2; exit 1 ;;
esac

case "$PLATFORM" in
  all|macos|windows) ;;
  *) echo "PLATFORM 必须是 all / macos / windows" >&2; exit 1 ;;
esac

# ---- 依赖 ----
VENV_PY="$REPO_ROOT/tools/package/.venv/bin/python"
[ -x "$VENV_PY" ] || {
  echo "缺少 Python 依赖：先执行一次 tools/release/publish_macos.sh（或创建 .venv 并安装 requirements.txt）" >&2
  exit 1
}
[ -f "$MANIFEST" ] || {
  echo "清单不存在：$MANIFEST" >&2
  echo "可先从更新源取回已签名清单：" >&2
  echo "  curl -fsS $CDN_DOMAIN/$PRODUCT/updates/$CHANNEL/manifest.json -o dist/manifest.json" >&2
  exit 1
}
[ -f "$SIGNING_KEY" ] || {
  echo "签名私钥不存在：$SIGNING_KEY" >&2
  exit 1
}
SIGNING_PUB="${SIGNING_KEY%.pem}.pub.pem"
[ -f "$SIGNING_PUB" ] || {
  echo "公钥不存在：$SIGNING_PUB" >&2
  exit 1
}

manifest_info() {  # manifest_info -> "<version> <build>"（all 取第一个平台，仅用于展示）
  "$VENV_PY" - "$MANIFEST" "$PLATFORM" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
platforms = body.get("platforms") or {}
name = sys.argv[2]
if name == "all":
    name = next(iter(platforms), "")
section = platforms.get(name) or {}
print(section.get("version", ""), section.get("build", 0))
PY
}

MANIFEST_CHANNEL="$("$VENV_PY" - "$MANIFEST" <<'PY'
import json, sys
print(json.load(open(sys.argv[1], encoding="utf-8")).get("channel", ""))
PY
)"
[ "$MANIFEST_CHANNEL" = "$CHANNEL" ] || {
  echo "清单通道是 ${MANIFEST_CHANNEL}，与 --channel ${CHANNEL} 不一致" >&2
  exit 1
}
if [ "$PLATFORM" = "all" ]; then
  VERSION="（全部平台）"
  BUILD=""
else
  read -r VERSION BUILD <<< "$(manifest_info)"
fi

# 先验原清单签名，避免在损坏或被篡改的清单上放量。
"$VENV_PY" "$REPO_ROOT/tools/release/manifest_v2.py" verify \
  --manifest "$MANIFEST" --pub "$SIGNING_PUB" >/dev/null

# ---- 节奏校验 ----
parse_duration() {  # parse_duration 24h / 90m / 30s / 2d / 纯秒数
  local raw="$1" unit="" num=""
  case "$raw" in
    *[smhd]) unit="${raw: -1}"; num="${raw%?}" ;;
    *) num="$raw" ;;
  esac
  [[ "$num" =~ ^[0-9]+$ ]] || return 1
  case "$unit" in
    d) echo $(( num * 86400 )) ;;
    h) echo $(( num * 3600 )) ;;
    m) echo $(( num * 60 )) ;;
    s|"") echo "$num" ;;
    *) return 1 ;;
  esac
}

INTERVAL_SECONDS="$(parse_duration "$INTERVAL" || true)"
[ -n "$INTERVAL_SECONDS" ] && [ "$INTERVAL_SECONDS" -gt 0 ] || {
  echo "无法解析 --interval：${INTERVAL}（支持 30s / 90m / 24h / 2d）" >&2
  exit 1
}

IFS=',' read -r -a STEP_LIST <<< "$STEPS"
[ "${#STEP_LIST[@]}" -gt 0 ] || { echo "--steps 不能为空" >&2; exit 1; }
PREV=0
for step in "${STEP_LIST[@]}"; do
  [[ "$step" =~ ^[0-9]+$ ]] || { echo "非数字档位：$step" >&2; exit 1; }
  [ "$step" -ge 1 ] && [ "$step" -le 100 ] || {
    echo "档位必须在 1–100：$step" >&2; exit 1; }
  [ "$step" -gt "$PREV" ] || { echo "档位必须严格递增：$STEPS" >&2; exit 1; }
  PREV="$step"
done
[ "${STEP_LIST[${#STEP_LIST[@]}-1]}" = "100" ] || {
  echo "最后一档必须是 100：$STEPS" >&2; exit 1; }

MANIFEST_KEY="$PRODUCT/updates/$CHANNEL/manifest.json"
MANIFEST_PUBLIC="$CDN_DOMAIN/$MANIFEST_KEY"
ARCHIVE_DIR="$REPO_ROOT/dist/rollout/${PRODUCT}-${CHANNEL}-$(date +%Y%m%d%H%M%S)"
mkdir -p "$ARCHIVE_DIR"

echo "===== 自动逐档放量 ====="
echo "产品 / 通道：   $PRODUCT / $CHANNEL"
echo "平台：          $PLATFORM"
echo "版本 / build：  ${VERSION}${BUILD:+ / $BUILD}"
echo "清单：          $MANIFEST"
echo "档位：          ${STEPS}（每档 ${INTERVAL}）"
echo "更新源：        $MANIFEST_PUBLIC"
echo "归档：          $ARCHIVE_DIR"
[ "$DRY_RUN" = "true" ] && echo "(dry-run：只重签，不上传、不等待)"

if [ "$DRY_RUN" != "true" ] && [ "$ASSUME_YES" != "true" ]; then
  if [ -t 0 ]; then
    printf '将按上述节奏修改线上清单，确认继续？输入 yes：'
    read -r answer
    [ "$answer" = "yes" ] || { echo "已取消"; exit 1; }
  else
    echo "非交互环境需显式加 --yes 才会修改线上清单" >&2
    exit 1
  fi
fi

# ---- OSS 凭据与上传 ----
cred_value() {  # cred_value <文件> <键名>
  sed -n "s/^[[:space:]]*\(export[[:space:]]\{1,\}\)\{0,1\}$2[[:space:]]*=[[:space:]]*//p" "$1" | tail -1 \
    | tr -d '\r' | sed -e 's/^"//' -e 's/"$//' -e "s/^'//" -e "s/'$//"
}

load_oss_credentials() {
  if [ -n "${OSS_ACCESS_KEY_ID:-}" ] && [ -n "${OSS_ACCESS_KEY_SECRET:-}" ]; then
    OSS_CRED_SOURCE="环境变量"
  else
    [ -f "$OSS_CRED_FILE" ] || {
      echo "缺少 OSS 凭据：请设置 OSS_ACCESS_KEY_ID / OSS_ACCESS_KEY_SECRET，" >&2
      echo "或写入仓库外的 ${OSS_CRED_FILE}（权限 600）。" >&2
      exit 1
    }
    local perm
    perm="$(stat -c %a "$OSS_CRED_FILE" 2>/dev/null || stat -f %Lp "$OSS_CRED_FILE" 2>/dev/null || echo unknown)"
    case "$perm" in
      600|400) ;;
      *) echo "凭据文件权限过宽（${perm}）：先 chmod 600 $OSS_CRED_FILE 再重试。" >&2; exit 1 ;;
    esac
    OSS_ACCESS_KEY_ID="$(cred_value "$OSS_CRED_FILE" OSS_ACCESS_KEY_ID)"
    OSS_ACCESS_KEY_SECRET="$(cred_value "$OSS_CRED_FILE" OSS_ACCESS_KEY_SECRET)"
    [ -n "$OSS_ACCESS_KEY_ID" ] && [ -n "$OSS_ACCESS_KEY_SECRET" ] || {
      echo "凭据文件缺少 OSS_ACCESS_KEY_ID / OSS_ACCESS_KEY_SECRET：$OSS_CRED_FILE" >&2
      exit 1
    }
    OSS_CRED_SOURCE="$(basename "$OSS_CRED_FILE")"
  fi
}

oss_upload() {  # oss_upload <本地文件> <object key>
  OSS_BUCKET="$OSS_BUCKET" OSS_REGION="$OSS_REGION" \
  OSS_ACCESS_KEY_ID="$OSS_ACCESS_KEY_ID" OSS_ACCESS_KEY_SECRET="$OSS_ACCESS_KEY_SECRET" \
  "$OSS_PY" - "$1" "$2" <<'PY'
import os, sys, oss2
local, key = sys.argv[1:3]
bucket = os.environ["OSS_BUCKET"]
region = os.environ["OSS_REGION"]
bkt = oss2.Bucket(
    oss2.Auth(os.environ["OSS_ACCESS_KEY_ID"], os.environ["OSS_ACCESS_KEY_SECRET"]),
    f"https://oss-{region}.aliyuncs.com",
    bucket,
)
with open(local, "rb") as f:
    bkt.put_object(key, f, headers={
        "Cache-Control": "no-cache, max-age=60",
        "Content-Type": "application/json",
    })
print(f"  ✓ 已上传 {key}")
PY
}

purge_manifest() {
  if command -v aliyun >/dev/null 2>&1; then
    if aliyun cdn RefreshObjectCaches --ObjectPath "$MANIFEST_PUBLIC" >/dev/null 2>&1; then
      echo "  ✓ 已 purge $MANIFEST_PUBLIC"
    else
      echo "  ! purge 失败，请手动刷新：$MANIFEST_PUBLIC"
    fi
  else
    echo "  (未安装 aliyun CLI，请手动 purge：$MANIFEST_PUBLIC)"
  fi
}

poll_manifest() {  # poll_manifest <期望比例>
  local want="$1" deadline=$((SECONDS + POLL_TIMEOUT)) state=""
  while [ "$SECONDS" -lt "$deadline" ]; do
    # 输出各平台当前比例；全部等于期望值时退出码 0。
    if state="$(curl -fsS --max-time 15 "$MANIFEST_PUBLIC?t=$(date +%s)" 2>/dev/null \
      | "$VENV_PY" -c 'import json,sys
body = json.load(sys.stdin)
platforms = body.get("platforms") or {}
name, want = sys.argv[1], int(sys.argv[2])
values = {k: int(v.get("rollout_percent", 100)) for k, v in platforms.items()}
if name != "all":
    values = {k: v for k, v in values.items() if k == name}
print(" ".join(f"{k}={v}%" for k, v in sorted(values.items())) or "<空>")
sys.exit(0 if values and all(v == want for v in values.values()) else 1)' \
      "$PLATFORM" "$want" 2>/dev/null)"; then
      echo "  ✓ CDN 已生效：${state}"
      return 0
    fi
    sleep 5
  done
  echo "  ✗ 等待 CDN 生效超时（期望 ${want}%，最后拿到 ${state:-<空>}），停止后续放量" >&2
  echo "    当前档位保持生效；排查 CDN/purge 后可从该档继续。" >&2
  return 1
}

if [ "$DRY_RUN" != "true" ]; then
  [ -n "${OSS_BUCKET:-}" ] && [ -n "${OSS_REGION:-}" ] || {
    echo "缺少必填：OSS_BUCKET / OSS_REGION" >&2
    exit 1
  }
  load_oss_credentials
  OSS_PY="${OSS_PY:-$HOME/.venv-oss/bin/python}"
  [ -x "$OSS_PY" ] || OSS_PY="$VENV_PY"
  if ! "$OSS_PY" -c "import oss2" 2>/dev/null; then
    echo "未找到 oss2 Python SDK，正在安装..."
    python3 -m venv "$HOME/.venv-oss"
    "$HOME/.venv-oss/bin/pip" install -q oss2
    OSS_PY="$HOME/.venv-oss/bin/python"
  fi
  echo "OSS：           $OSS_BUCKET / ${OSS_REGION}（凭据：${OSS_CRED_SOURCE}）"
fi

# ---- 逐档执行 ----
TOTAL="${#STEP_LIST[@]}"
INDEX=0
for step in "${STEP_LIST[@]}"; do
  INDEX=$((INDEX + 1))
  OUT="$ARCHIVE_DIR/manifest-${step}.json"
  echo ""
  echo "===== [$INDEX/$TOTAL] rollout=${step}%（${PLATFORM}）"
  "$VENV_PY" "$REPO_ROOT/tools/release/manifest_v2.py" resign \
    --manifest "$MANIFEST" --key "$SIGNING_KEY" --rollout "$step" \
    --platform "$PLATFORM" --out "$OUT"
  "$VENV_PY" "$REPO_ROOT/tools/release/manifest_v2.py" verify \
    --manifest "$OUT" --pub "$SIGNING_PUB" >/dev/null
  echo "  ✓ 已重签并验签：$OUT"

  if [ "$DRY_RUN" = "true" ]; then
    continue
  fi

  oss_upload "$OUT" "$MANIFEST_KEY"
  purge_manifest
  poll_manifest "$step"

  if [ "$INDEX" -lt "$TOTAL" ]; then
    echo "  … 观察 ${INTERVAL}（Ctrl-C 可停在当前档）"
    sleep "$INTERVAL_SECONDS"
  fi
done

echo ""
echo "================ 放量完成 ================"
echo "版本 / build：  ${VERSION}${BUILD:+ / $BUILD}"
echo "最终比例：      ${STEP_LIST[${#STEP_LIST[@]}-1]}%（${PLATFORM}）"
echo "清单：          $MANIFEST_PUBLIC"
echo "每档归档：      $ARCHIVE_DIR"
echo "回滚：把上一档 manifest-<比例>.json 重新上传并 purge 即可"
