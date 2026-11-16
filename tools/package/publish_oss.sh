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
#   oss://<bucket>/<product>/updates/<channel>/latest.json  ← 60 秒缓存
#   oss://<bucket>/<product>/updates/<channel>/appcast.xml  ← 60 秒缓存
#
# 发布后还会同步桶根目录 index.html 里该产品的字节数 / SHA256（UPDATE_INDEX_PAGE=false 可关闭），
# 并下载一次新包预热 CDN 边缘缓存（WARMUP=false 关闭）。
#
# 凭据（AccessKey）：
#   1. 优先读环境变量 OSS_ACCESS_KEY_ID / OSS_ACCESS_KEY_SECRET；
#   2. 未设置环境变量时读仓库外的 ~/.config/xiaotu-updates/oss-credentials.env
#      （两行键值，权限必须为 600 或 400；可用 OSS_CRED_FILE 改路径）。
#   密钥只授予该桶的最小权限，绝不写进仓库、脚本或 CI 日志；若曾以明文出现在
#   聊天、命令行历史或日志里，请立即到 RAM 控制台轮换。发布前脚本会检查受版本
#   控制的文件里是否残留当前 AccessKey，命中即拒绝发布。
#
# 依赖：ossutil、python3 + cryptography（tools/package/.venv）、openssl、shasum、PlistBuddy
#
# 用法：
#   # 完整发布（需要 ossutil + 预配置 bucket）
#   OSS_BUCKET=xxx OSS_REGION=cn-hangzhou PRODUCT=xiaotu tools/package/publish_oss.sh
#
#   # dry-run：只本地生成 + 验签，不碰 OSS
#   OSS_SKIP=true PRODUCT=xiaotu tools/package/publish_oss.sh
#
#   # 关闭下载页数字同步
#   UPDATE_INDEX_PAGE=false PRODUCT=xiaotu tools/package/publish_oss.sh

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
# 发布后自动把 index.html 里小兔包的字节数/SHA256 改成新包的值
UPDATE_INDEX_PAGE="${UPDATE_INDEX_PAGE:-true}"
# 发布后下载一次 dmg 预热 CDN 边缘缓存（WARMUP=false 关闭，WARM_TIMEOUT 单位秒）
WARMUP="${WARMUP:-true}"
WARM_TIMEOUT="${WARM_TIMEOUT:-300}"

# ---- OSS 凭据：环境变量优先，其次仓库外 600 权限的凭据文件 ----
# 凭据只在本进程内使用，不导出给子进程：上传时按参数传给 Python。
OSS_CRED_FILE="${OSS_CRED_FILE:-$HOME/.config/xiaotu-updates/oss-credentials.env}"

cred_value() {  # cred_value <文件> <键名>
  sed -n "s/^[[:space:]]*$2[[:space:]]*=[[:space:]]*//p" "$1" | tail -1 \
    | tr -d '\r' | sed -e 's/^"//' -e 's/"$//' -e "s/^'//" -e "s/'$//"
}

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
      echo "或写入仓库外的 $OSS_CRED_FILE（权限 600）。" >&2
      exit 1
    fi
    local perm
    perm="$(stat -c %a "$OSS_CRED_FILE" 2>/dev/null || stat -f %Lp "$OSS_CRED_FILE" 2>/dev/null || echo unknown)"
    case "$perm" in
      600|400) ;;
      *) echo "凭据文件权限过宽（$perm）：先 chmod 600 $OSS_CRED_FILE 再重试。" >&2; exit 1 ;;
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
  load_oss_credentials
  assert_cred_not_tracked
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
echo "AK / 来源：               ${AK_MASK:-<dry-run>} / ${OSS_CRED_SOURCE:-<dry-run>}"
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

# ---- ⑥ 同步下载页数字 ----
# index.html 里小兔包卡片硬编码了字节数与 SHA256，发布后自动改写，避免每次手改。
if [ "$OSS_SKIP" != "true" ] && [ "$UPDATE_INDEX_PAGE" = "true" ]; then
  echo ""
  echo "===== ⑥ 同步下载页 index.html（小兔包大小与 SHA256）"
  "$OSS_PY" - "$OSS_BUCKET" "$OSS_REGION" "$OSS_ACCESS_KEY_ID" "$OSS_ACCESS_KEY_SECRET" \
    "$DMG_SIZE" "$DMG_SHA256" <<'PY_EOF'
import re
import sys
import oss2

bucket_name, region, ak, sk, size_s, sha = sys.argv[1:]
size = int(size_s)
bkt = oss2.Bucket(oss2.Auth(ak, sk), f"https://oss-{region}.aliyuncs.com", bucket_name)
try:
    html = bkt.get_object("index.html").read().decode("utf-8")
except oss2.exceptions.NoSuchKey:
    print("  跳过：桶根目录没有 index.html")
    raise SystemExit(0)

idx = html.find('id="sha-xiaotu-mac"')
if idx < 0:
    print('  跳过：index.html 里没有 id="sha-xiaotu-mac"')
    raise SystemExit(0)

before = html[:idx]
pattern = re.compile(r'(<span class="mono">)\d[\d,]*(</span> 字节（约 )[\d.]+( MB）)')
matches = list(pattern.finditer(before))
if not matches:
    print("  跳过：未找到小兔包的字节数展示位")
    raise SystemExit(0)
m = matches[-1]
new_size = f"{size:,}"
new_mb = f"{size / (1024 * 1024):.1f}"
before = (before[:m.start()] + m.group(1) + new_size + m.group(2) + new_mb +
          m.group(3) + before[m.end():])

after = html[idx:]
after = re.sub(r'(id="sha-xiaotu-mac"\s+title=")[0-9a-f]{64}(")',
               r'\g<1>' + sha + r'\g<2>', after, count=1)
after = re.sub(r'(id="sha-xiaotu-mac"[^>]*>)[0-9a-f]{64}(</code>)',
               r'\g<1>' + sha + r'\g<2>', after, count=1)

html2 = before + after
if html2 == html:
    print(f"  无需更新：下载页已是 {new_size} 字节 / {new_mb} MB")
else:
    bkt.put_object("index.html", html2.encode("utf-8"),
                   headers={"Content-Type": "text/html; charset=utf-8",
                            "Cache-Control": "no-cache, max-age=60"})
    print(f"  ✓ index.html 已更新：{new_size} 字节 / {new_mb} MB / {sha[:16]}…")
PY_EOF
else
  echo ""
  echo "===== ⑥ [跳过] 下载页数字同步（UPDATE_INDEX_PAGE=false 或 dry-run）"
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
      echo "  ! 预热未完成（curl exit=$WARM_RC，HTTP ${WARM_CODE:-?}）：$(tr '\n' ' ' <"$WARM_ERR")"
    fi
    rm -f "$WARM_ERR"
  else
    echo "  (未安装 curl，跳过预热)"
  fi
else
  echo ""
  echo "===== ⑦ [跳过] CDN 预热（WARMUP=false 或 dry-run）"
fi

# ---- ⑧ 完成 ----
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
if [ "${OSS_CRED_SOURCE:-}" = "环境变量" ]; then
  echo "→ 提醒：本次凭据来自环境变量。若曾明文粘贴到聊天、命令行历史或日志，"
  echo "  请到 RAM 控制台轮换这对密钥，并改用 $OSS_CRED_FILE（chmod 600）。"
  echo ""
fi
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
