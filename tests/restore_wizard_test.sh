#!/usr/bin/env bash
# 恢复向导端到端：列出备份 → 预检查 → 必须二次确认 → 提交后进程退出 →
# 重启时完成恢复、写 restore 审计、保留恢复前安全备份。
# 用法：restore_wizard_test.sh <xiaotu-server 可执行文件>
set -uo pipefail

BIN=${1:?用法: restore_wizard_test.sh <xiaotu-server 可执行文件>}
if [ ! -x "$BIN" ]; then
  echo "FAIL: 找不到可执行文件 $BIN"
  exit 1
fi

ROOT=$(mktemp -d "${TMPDIR:-/tmp}/xiaotu-restore-wizard.XXXXXX")
DATA="$ROOT/data"
PORT=""
PID=""
TOKEN=""
CSRF=""
COOKIES="$ROOT/cookies"

fail() {
  echo "FAIL: $*"
  [ -f "$ROOT/server.log" ] && tail -40 "$ROOT/server.log"
  exit 1
}

cleanup() {
  if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
    kill "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
  fi
  rm -rf "$ROOT"
}
trap cleanup EXIT

mkdir -p "$DATA"
XIAOTU_PASSWORD=Restore0rd! "$BIN" --data "$DATA" --set-password admin >"$ROOT/init.out" 2>&1 ||
  fail "初始化数据库失败：$(cat "$ROOT/init.out")"
"$BIN" --data "$DATA" --backup >"$ROOT/backup.out" 2>&1 ||
  fail "生成手动备份失败：$(cat "$ROOT/backup.out")"
BACKUP=$(ls "$DATA"/backups/*-manual.db 2>/dev/null | head -1)
[ -n "$BACKUP" ] || fail "备份目录内没有 manual 备份"
BACKUP_NAME=$(basename "$BACKUP")

start_server() {
  : >"$ROOT/server.log"
  "$BIN" --data "$DATA" >"$ROOT/server.log" 2>&1 &
  PID=$!
  PORT=""
  for _ in $(seq 1 80); do
    PORT=$(sed -n 's|.*服务地址: http://127.0.0.1:\([0-9]*\).*|\1|p' "$ROOT/server.log" | head -1)
    [ -n "$PORT" ] && break
    kill -0 "$PID" 2>/dev/null || fail "服务进程提前退出"
    sleep 0.25
  done
  [ -n "$PORT" ] || fail "未能解析服务端口"
  TOKEN=$(sed -n 's|.*#t=\([0-9a-f]*\).*|\1|p' "$ROOT/server.log" | head -1)
  [ -n "$TOKEN" ] || fail "未能解析单机访问令牌"
  local response
  response=$(curl -s -c "$COOKIES" -X POST -H 'Content-Type: application/json' \
    -d "{\"token\":\"$TOKEN\"}" "http://127.0.0.1:$PORT/api/session")
  CSRF=$(echo "$response" | sed -n 's|.*"csrf":"\([^"]*\)".*|\1|p')
  [ -n "$CSRF" ] || fail "单机令牌换会话失败：$response"
  # 首次登录仍是初始口令，必须先完成强制改密才能调用其他接口。
  if echo "$response" | grep -q '"must_change_password":true'; then
    response=$(curl -s -c "$COOKIES" -b "$COOKIES" -X POST \
      -H 'Content-Type: application/json' -H "X-CSRF-Token: $CSRF" \
      -d '{"old_password":"Restore0rd!","new_password":"Restore0rd2!"}' \
      "http://127.0.0.1:$PORT/api/password/change")
    CSRF=$(echo "$response" | sed -n 's|.*"csrf":"\([^"]*\)".*|\1|p')
    [ -n "$CSRF" ] || fail "强制改密失败：$response"
  fi
}

api_post() {
  curl -s -b "$COOKIES" -X POST -H 'Content-Type: application/json' \
    -H "X-CSRF-Token: $CSRF" -d "$2" "http://127.0.0.1:$PORT$1"
}

start_server

# 1. 备份列表与预检查。
list=$(curl -s -b "$COOKIES" "http://127.0.0.1:$PORT/api/restore/backups")
echo "$list" | grep -q "$BACKUP_NAME" || fail "备份列表缺少 ${BACKUP_NAME}：$list"
echo "$list" | grep -q '"readable":true' || fail "备份应可读：$list"
precheck=$(api_post /api/restore/precheck "{\"file\":\"$BACKUP_NAME\"}")
echo "$precheck" | grep -q '"ok":true' || fail "预检查应通过：$precheck"
echo "$precheck" | grep -q '"is_app_database":true' || fail "预检查应识别本程序数据库：$precheck"

# 越界文件名必须拒绝。
unsafe=$(api_post /api/restore/precheck '{"file":"../app.db"}')
echo "$unsafe" | grep -q 'INVALID_FILE_NAME' || fail "路径穿越应被拒绝：$unsafe"

# 2. 备份之后写入一条商品，恢复后应消失。
created=$(api_post /api/items '{"code":"RST-ITEM","name":"恢复前新增","unit_id":1}')
echo "$created" | grep -q '"code":"RST-ITEM"' || fail "创建测试商品失败：$created"
items_before=$(curl -s -b "$COOKIES" "http://127.0.0.1:$PORT/api/items")
echo "$items_before" | grep -q 'RST-ITEM' || fail "恢复前应能查到测试商品"

# 3. 未带确认必须拒绝。
no_confirm=$(api_post /api/restore/apply "{\"file\":\"$BACKUP_NAME\"}")
echo "$no_confirm" | grep -q 'RESTORE_CONFIRM_REQUIRED' || fail "缺 confirm 应拒绝：$no_confirm"

# 4. 确认后进入维护态并退出。
applied=$(api_post /api/restore/apply "{\"file\":\"$BACKUP_NAME\",\"confirm\":true}")
echo "$applied" | grep -q '"state":"restarting"' || fail "恢复提交应返回 restarting：$applied"
for _ in $(seq 1 60); do
  kill -0 "$PID" 2>/dev/null || break
  sleep 0.25
done
kill -0 "$PID" 2>/dev/null && fail "提交恢复后服务未退出"
wait "$PID" 2>/dev/null
PID=""

# 5. 重启：结果、数据回退、审计、安全备份。
start_server
result=$(curl -s -b "$COOKIES" "http://127.0.0.1:$PORT/api/restore/result")
echo "$result" | grep -q '"ok":true' || fail "恢复结果应为成功：$result"
echo "$result" | grep -q "$BACKUP_NAME" || fail "恢复结果应带备份文件名：$result"
items_after=$(curl -s -b "$COOKIES" "http://127.0.0.1:$PORT/api/items")
if echo "$items_after" | grep -q 'RST-ITEM'; then
  fail "恢复后不应再看到备份之后新增的商品"
fi
audit=$(curl -s -b "$COOKIES" "http://127.0.0.1:$PORT/api/audit-logs?page_size=50")
echo "$audit" | grep -q '"action":"restore"' || fail "恢复后应写 restore 审计：$audit"
ls "$DATA"/backups/pre-restore-*.db >/dev/null 2>&1 || fail "缺少恢复前安全备份"

echo "restore_wizard_test 全部通过"
