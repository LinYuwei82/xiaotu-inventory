#!/usr/bin/env bash
# 小兔在线 · OSS 旧版本清理脚本
#
# 清理 dl.xiaotu.online 上各产品历史 dmg 包，只保留最近 N 个 stable 版本。
# latest.json / appcast.xml 永远只一份（当前索引），manual.pdf 固定名，均不碰。
#
# 三款产品的 dmg 文件名格式不同，脚本内置各自的版本号正则：
#   xiaotu:   xiaotu-inventory-1.0.0.dmg       → 1.0.0
#   devcard:  DevCard-1.2.2-macos-arm64.dmg    → 1.2.2
#   mote:     Mote-macOS-native-0.3.11.dmg     → 0.3.11
#
# 凭据加载逻辑与 publish_oss.sh / rollout_oss.sh 一致：
#   1. 环境变量 OSS_ACCESS_KEY_ID / OSS_ACCESS_KEY_SECRET
#   2. 否则读 ~/.config/xiaotu-updates/oss-credentials.env（权限 600/400）
#
# 安全保护：
#   - 默认 dry-run，先看删哪些再决定
#   - 非交互环境（CI / 管道）必须显式 --force
#   - 永远至少保留 1 个版本（不会把整个产品清空）
#   - latest.json / appcast.xml / manual.pdf / index.html / 404.html / logs/ 永远不动
#
# 用法：
#   # 先 dry-run 看要删哪些（推荐每次先跑这个）
#   tools/package/cleanup_oss_versions.sh --dry-run
#
#   # 保留最近 3 个（默认），真删
#   tools/package/cleanup_oss_versions.sh --force
#
#   # 小兔只保留 1 个（激进）
#   tools/package/cleanup_oss_versions.sh --product xiaotu --keep 1 --force
#
#   # 全产品但每个都只保留 2 个
#   tools/package/cleanup_oss_versions.sh --keep 2 --force

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

usage() {
  sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}
FORCE=false
DRY_RUN=false
KEEP=3
PRODUCT="all"

while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --force)   FORCE=true ;;
    --dry-run) DRY_RUN=true ;;
    --keep)    KEEP="${2:-}"; shift ;;
    --product) PRODUCT="${2:-}"; shift ;;
    *) echo "未知参数：$1（用 --help 查看用法）" >&2; exit 2 ;;
  esac
  shift
done

if ! [[ "$KEEP" =~ ^[0-9]+$ ]] || [ "$KEEP" -lt 1 ]; then
  echo "--keep 必须是 ≥ 1 的整数" >&2; exit 1
fi
case "$PRODUCT" in
  xiaotu|devcard|mote|all) ;;
  *) echo "--product 必须是 xiaotu / devcard / mote / all" >&2; exit 1 ;;
esac

# ---- 非交互强制 --force 保护 ----
if [ "$FORCE" != "true" ] && [ "$DRY_RUN" != "true" ]; then
  if [ ! -t 0 ]; then
    echo "非交互环境必须显式 --force 或 --dry-run，拒绝静默误删。" >&2
    echo "用 --help 查看用法。" >&2
    exit 1
  fi
fi

# ---- OSS 凭据加载 ----
OSS_BUCKET="${OSS_BUCKET:-}"
OSS_REGION="${OSS_REGION:-}"
OSS_CRED_FILE="${OSS_CRED_FILE:-$HOME/.config/xiaotu-updates/oss-credentials.env}"

if [ -z "$OSS_BUCKET" ] || [ -z "$OSS_REGION" ]; then
  echo "缺少必填：OSS_BUCKET / OSS_REGION（或 source ~/.config/xiaotu-updates/.env）" >&2; exit 1
fi

VENV_PY="$REPO_ROOT/tools/package/.venv/bin/python"
if [ ! -x "$VENV_PY" ]; then
  python3 -m venv "$REPO_ROOT/tools/package/.venv"
  "$REPO_ROOT/tools/package/.venv/bin/pip" install -q -r "$REPO_ROOT/tools/package/requirements.txt"
fi
if ! "$VENV_PY" -c "import oss2" 2>/dev/null; then
  "$REPO_ROOT/tools/package/.venv/bin/pip" install -q oss2
fi

cred_value() {
  sed -n "s/^[[:space:]]*$2[[:space:]]*=[[:space:]]*//p" "$1" | tail -1 \
    | tr -d '\r' | sed -e 's/^"//' -e 's/"$//' -e "s/^'//" -e "s/'$//"
}
load_oss_creds() {
  if [ -n "${OSS_ACCESS_KEY_ID:-}" ] && [ -n "${OSS_ACCESS_KEY_SECRET:-}" ]; then
    return 0
  fi
  if [ ! -f "$OSS_CRED_FILE" ]; then
    echo "缺少 OSS 凭据：请设置 OSS_ACCESS_KEY_ID / OSS_ACCESS_KEY_SECRET" >&2
    echo "或写入仓库外的 $OSS_CRED_FILE（权限 600）。" >&2; exit 1
  fi
  local perm
  perm="$(stat -c %a "$OSS_CRED_FILE" 2>/dev/null || stat -f %Lp "$OSS_CRED_FILE" 2>/dev/null || echo unknown)"
  case "$perm" in 600|400) ;; *) echo "凭据文件权限过宽（$perm）" >&2; exit 1 ;; esac
  OSS_ACCESS_KEY_ID="$(cred_value "$OSS_CRED_FILE" OSS_ACCESS_KEY_ID)"
  OSS_ACCESS_KEY_SECRET="$(cred_value "$OSS_CRED_FILE" OSS_ACCESS_KEY_SECRET)"
  [ -n "$OSS_ACCESS_KEY_ID" ] && [ -n "$OSS_ACCESS_KEY_SECRET" ] || \
    { echo "凭据文件缺少键值" >&2; exit 1; }
  export OSS_ACCESS_KEY_ID OSS_ACCESS_KEY_SECRET
}
load_oss_creds

# ---- 执行清理 ----
MODE_LABEL="真删"
[ "$DRY_RUN" = "true" ] && MODE_LABEL="dry-run"
[ "$FORCE" = "true" ] && MODE_LABEL="真删"

echo "===== OSS 旧版本清理 · ${MODE_LABEL} ====="
echo "bucket / region： ${OSS_BUCKET} / ${OSS_REGION}"
echo "product：         ${PRODUCT}"
echo "保留最近：        ${KEEP} 个版本"
echo ""

"$VENV_PY" - "$OSS_BUCKET" "$OSS_REGION" "$OSS_ACCESS_KEY_ID" "$OSS_ACCESS_KEY_SECRET" \
  "$PRODUCT" "$KEEP" "$FORCE" "$DRY_RUN" <<'PY_EOF'
import sys, oss2, re, semver
from datetime import datetime

bucket, region, ak, sk, product_arg, keep_s, force_s, dry_s = sys.argv[1:]
keep = int(keep_s)
force = force_s == "True"
dry = dry_s == "True"

auth = oss2.Auth(ak, sk)
bkt = oss2.Bucket(auth, f"https://oss-{region}.aliyuncs.com", bucket)

# 各产品版本号正则：从 dmg 文件名提取 semver
PRODUCT_PATTERNS = {
    "xiaotu":  re.compile(r"xiaotu-inventory-(\d+\.\d+\.\d+.*)\.dmg$"),
    "devcard": re.compile(r"DevCard-(\d+\.\d+\.\d+.*)-macos.*\.dmg$"),
    "mote":    re.compile(r"Mote-macOS-native-(\d+\.\d+\.\d+.*)\.dmg$"),
}

def parse_version(vstr):
    """把 semver 字符串转成可比的 tuple。支持 1.0.0 和 1.0.0-beta 等。"""
    try:
        sv = semver.VersionInfo.parse(vstr)
        return (sv.major, sv.minor, sv.patch, sv.prerelease or "")
    except ValueError:
        # 兜底：数字逐段拆分
        parts = vstr.split("-")[0].split(".")
        nums = [int(p) if p.isdigit() else 0 for p in (parts + ["0", "0", "0"])[:3]]
        return tuple(nums + [vstr.split("-")[1] if "-" in vstr else ""])

def list_dmgs(prefix):
    """列出 prefix/ 目录下所有 .dmg（精确匹配前缀，不抓 devcard-test 这种）。"""
    results = []
    for obj in oss2.ObjectIterator(bkt, prefix=prefix + "/"):
        if obj.key.endswith(".dmg"):
            results.append(obj)
    return results

def fmt_date(ts):
    try:
        return datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M")
    except Exception:
        return str(ts)

products = ["xiaotu", "devcard", "mote"] if product_arg == "all" else [product_arg]

total_kept = 0
total_deleted = 0
total_freed = 0
errors = []

for prod in products:
    print(f"--- {prod} ---")
    pattern = PRODUCT_PATTERNS[prod]
    prefix = prod
    dmgs = list_dmgs(prefix)

    if not dmgs:
        print(f"  （没有 dmg 文件，跳过）\n")
        continue

    # 解析版本号 + 过滤
    parsed = []
    unrecognized = []
    for obj in dmgs:
        m = pattern.search(obj.key)
        if m:
            vstr = m.group(1)
            parsed.append((parse_version(vstr), vstr, obj))
        else:
            # 不匹配任何已知正则的 dmg 不动（保守）
            unrecognized.append(obj)

    # 按版本号降序排，保留前 keep 个
    parsed.sort(key=lambda x: x[0], reverse=True)
    to_keep = parsed[:keep]
    to_delete = parsed[keep:]

    # 输出表格
    print(f"  共 {len(dmgs)} 个 dmg，保留最近 {len(to_keep)} 个，清理 {len(to_delete)} 个")
    print(f"  {'版本':16s} {'大小':>10s}  {'上传时间':>16s}  {'动作'}")
    print(f"  {'-'*16} {'-'*10}  {'-'*16}  {'----'}")

    for _, vstr, obj in to_keep:
        print(f"  {vstr:16s} {obj.size:>10,}  {fmt_date(obj.last_modified):>16s}  保留")
    for _, vstr, obj in to_delete:
        print(f"  {vstr:16s} {obj.size:>10,}  {fmt_date(obj.last_modified):>16s}  删除")

    for obj in unrecognized:
        print(f"  ? 无法识别版本号，保守跳过：{obj.key}")

    # 执行
    if to_delete:
        if force and not dry:
            print(f"  → 正在删除 {len(to_delete)} 个文件...")
            for _, vstr, obj in to_delete:
                try:
                    bkt.delete_object(obj.key)
                    print(f"    ✓ {obj.key}")
                    total_deleted += 1
                    total_freed += obj.size
                except Exception as e:
                    print(f"    ✗ 删除失败：{obj.key} → {e}")
                    errors.append((prod, obj.key, str(e)))
        elif dry or not force:
            if dry:
                print(f"  → [dry-run] 将删除 {len(to_delete)} 个（加 --force 真删）")
            else:
                print(f"  → 将删除 {len(to_delete)} 个（加 --force 真删）")
        print()
    else:
        print(f"  ✓ 无需清理（已经 ≤ {keep} 个）\n")

    total_kept += len(to_keep)

# ---- 汇总 ----
print("=" * 50)
print(f"合计：保留 {total_kept} 个，清理 {total_deleted} 个，释放 {total_freed:,} 字节", end="")
if total_deleted:
    print(f"（≈ {total_freed / 1048576:.1f} MB）")
else:
    print()
if errors:
    print(f"⚠ 有 {len(errors)} 个错误：")
    for prod, key, msg in errors:
        print(f"  {prod} / {key} → {msg}")
    sys.exit(1)
PY_EOF
