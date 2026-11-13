#!/usr/bin/env bash
# 验证 [storage] backup_dir 的端到端行为：绝对/相对目录、不可写报错、迁移前备份落点。
# 用法：backup_dir_test.sh <xiaotu-server 可执行文件>
set -uo pipefail

BIN=${1:?用法: backup_dir_test.sh <xiaotu-server 可执行文件>}
if [ ! -x "$BIN" ]; then
  echo "FAIL: 找不到可执行文件 $BIN"
  exit 1
fi

ROOT=$(mktemp -d "${TMPDIR:-/tmp}/xiaotu-backupdir-XXXXXX")
cleanup() {
  if [ -n "${MIGRATE_PID:-}" ]; then
    kill "$MIGRATE_PID" 2>/dev/null
    wait "$MIGRATE_PID" 2>/dev/null
  fi
  chmod -R u+rwX "$ROOT" 2>/dev/null
  rm -rf "$ROOT"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*"
  exit 1
}

manual_count() {
  local dir=$1
  local count=0
  local file
  for file in "$dir"/app-*-manual.db; do
    [ -e "$file" ] && count=$((count + 1))
  done
  echo "$count"
}

# 1. 先用 --set-password 生成一个当前版本的库。
DATA="$ROOT/data"
CONFIG="$ROOT/config.toml"
CUSTOM="$ROOT/custom-backups"
mkdir -p "$DATA"
XIAOTU_PASSWORD=Backup0rd! "$BIN" --data "$DATA" \
  --set-password admin >"$ROOT/init.out" 2>&1 ||
  fail "初始化数据库失败：$(cat "$ROOT/init.out")"
[ -f "$DATA/app.db" ] || fail "初始化后没有生成 app.db"

# 2. 绝对路径：备份写入自定义目录，不写默认目录。
cat >"$CONFIG" <<EOF
[storage]
backup_dir = "$CUSTOM"
EOF
"$BIN" --data "$DATA" --config "$CONFIG" --backup >"$ROOT/abs.out" 2>&1 ||
  fail "自定义绝对目录备份失败：$(cat "$ROOT/abs.out")"
[ "$(manual_count "$CUSTOM")" -ge 1 ] || fail "自定义绝对目录内没有 manual 备份"
[ -d "$DATA/backups" ] && fail "自定义目录生效时仍创建了默认 backups 目录"

# 3. 相对路径：按数据目录解析。
cat >"$CONFIG" <<EOF
[storage]
backup_dir = "rel-backups"
EOF
"$BIN" --data "$DATA" --config "$CONFIG" --backup >"$ROOT/rel.out" 2>&1 ||
  fail "相对目录备份失败：$(cat "$ROOT/rel.out")"
[ "$(manual_count "$DATA/rel-backups")" -ge 1 ] || fail "相对目录未按数据目录解析"

# 4. 不可写目录：必须报错且不回落到默认目录。
LOCKED="$ROOT/locked"
mkdir -p "$LOCKED"
chmod 0500 "$LOCKED"
cat >"$CONFIG" <<EOF
[storage]
backup_dir = "$LOCKED"
EOF
"$BIN" --data "$DATA" --config "$CONFIG" --backup >"$ROOT/locked.out" 2>&1
CODE=$?
chmod 0700 "$LOCKED"
[ "$CODE" -ne 0 ] || fail "备份目录不可写时 --backup 应返回非零"
grep -q "备份目录不可写" "$ROOT/locked.out" ||
  fail "不可写时输出缺少明确错误：$(cat "$ROOT/locked.out")"
[ -d "$DATA/backups" ] && fail "不可写时静默回落到了默认备份目录"

# 5. 迁移前备份：构造一个 v1 的库，触发迁移前快照。
OLD="$ROOT/old-data"
MIGRATE_BK="$ROOT/migrate-backups"
mkdir -p "$OLD"
OLD_DB="$OLD/app.db"
if command -v sqlite3 >/dev/null 2>&1; then
  sqlite3 "$OLD_DB" \
    "CREATE TABLE schema_migration (version INTEGER PRIMARY KEY, name TEXT NOT NULL, checksum TEXT NOT NULL, applied_at TEXT NOT NULL);
     INSERT INTO schema_migration VALUES (1, 'v1_init', 'smoke', '2026-01-01 00:00:00');
     PRAGMA user_version = 1;" >/dev/null ||
    fail "构造 v1 数据库失败"
else
  python3 - "$OLD_DB" <<'PY' || fail "构造 v1 数据库失败"
import sqlite3
import sys
conn = sqlite3.connect(sys.argv[1])
conn.executescript("""
CREATE TABLE schema_migration (version INTEGER PRIMARY KEY, name TEXT NOT NULL, checksum TEXT NOT NULL, applied_at TEXT NOT NULL);
INSERT INTO schema_migration VALUES (1, 'v1_init', 'smoke', '2026-01-01 00:00:00');
PRAGMA user_version = 1;
""")
conn.commit()
conn.close()
PY
fi

cat >"$CONFIG" <<EOF
[storage]
backup_dir = "$MIGRATE_BK"
EOF
"$BIN" --data "$OLD" --config "$CONFIG" >"$ROOT/migrate.out" 2>&1 &
MIGRATE_PID=$!
MIGRATED=0
for _ in $(seq 1 150); do
  for file in "$MIGRATE_BK"/app-*-pre-v1.db; do
    if [ -e "$file" ]; then
      MIGRATED=1
      break
    fi
  done
  [ "$MIGRATED" -eq 1 ] && break
  kill -0 "$MIGRATE_PID" 2>/dev/null || break
  sleep 0.1
done
kill "$MIGRATE_PID" 2>/dev/null
wait "$MIGRATE_PID" 2>/dev/null
MIGRATE_PID=""
[ "$MIGRATED" -eq 1 ] || fail "迁移前备份没有写入自定义目录：$(cat "$ROOT/migrate.out")"
[ -d "$OLD/backups" ] && fail "迁移前备份回落到默认目录"

echo "backup_dir_test 通过"
