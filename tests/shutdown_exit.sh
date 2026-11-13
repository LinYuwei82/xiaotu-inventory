#!/usr/bin/env bash
# 优雅关闭端到端：子进程启动 --server，校验 401/错误令牌、正确令牌返回 200 且含
# stopping、令牌文件被删除，随后进程自行退出（退出码 0）并留下 shutdown 审计。
# 用法：shutdown_exit.sh <xiaotu-server 可执行文件>
set -uo pipefail

BIN=${1:?用法: shutdown_exit.sh <xiaotu-server 可执行文件>}
if [ ! -x "$BIN" ]; then
  echo "FAIL: 找不到可执行文件 $BIN"
  exit 1
fi

DATA=$(mktemp -d "${TMPDIR:-/tmp}/xiaotu-shutdown-XXXXXX")
PORT=$((20000 + RANDOM % 20000))
OUT="$DATA/server.out"
TOKEN_FILE="$DATA/.shutdown-token"
SERVER_PID=""

fail() {
  echo "FAIL: $*"
  [ -f "$OUT" ] && cat "$OUT"
  exit 1
}

cleanup() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
  fi
  rm -rf "$DATA"
}
trap cleanup EXIT

"$BIN" --server --port "$PORT" --data "$DATA" >"$OUT" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 150); do
  [ -f "$TOKEN_FILE" ] && break
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    fail "测试服务未能启动"
  fi
  sleep 0.1
done
[ -f "$TOKEN_FILE" ] || fail "启动后未生成关闭令牌文件"

TOKEN=$(cat "$TOKEN_FILE")
[ -n "$TOKEN" ] || fail "关闭令牌文件为空"

MODE=$(stat -f '%Lp' "$TOKEN_FILE" 2>/dev/null || stat -c '%a' "$TOKEN_FILE" 2>/dev/null)
[ "$MODE" = "600" ] || fail "关闭令牌文件权限应为 600，实际为 ${MODE:-未知}"

# 启动日志与运行日志都不得出现令牌本身。
if grep -qF "$TOKEN" "$OUT"; then
  fail "启动日志泄露了关闭令牌"
fi
if [ -d "$DATA/logs" ] && grep -rqF "$TOKEN" "$DATA/logs"; then
  fail "运行日志泄露了关闭令牌"
fi

URL="http://127.0.0.1:$PORT/api/system/shutdown"

CODE=$(curl -s -o "$DATA/r1" -w '%{http_code}' -X POST "$URL")
[ "$CODE" = "401" ] || fail "无令牌应返回 401，实际 $CODE"
[ -f "$TOKEN_FILE" ] || fail "未授权请求不应删除令牌文件"

CODE=$(curl -s -o "$DATA/r2" -w '%{http_code}' -X POST \
  -H "X-Xiaotu-Shutdown-Token: wrong-token" "$URL")
[ "$CODE" = "401" ] || fail "错误令牌应返回 401，实际 $CODE"

CODE=$(curl -s -o "$DATA/r3" -w '%{http_code}' -X POST \
  -H "X-Xiaotu-Shutdown-Token: $TOKEN" "$URL")
[ "$CODE" = "200" ] || fail "正确令牌应返回 200，实际 $CODE"
grep -q "stopping" "$DATA/r3" || fail "响应体应包含 stopping"

[ ! -f "$TOKEN_FILE" ] || fail "令牌使用后文件应被删除"

for _ in $(seq 1 100); do
  kill -0 "$SERVER_PID" 2>/dev/null || break
  sleep 0.1
done
if kill -0 "$SERVER_PID" 2>/dev/null; then
  fail "服务在关闭请求后未及时退出"
fi

wait "$SERVER_PID"
CODE=$?
SERVER_PID=""
[ "$CODE" = "0" ] || fail "优雅关闭后进程退出码应为 0，实际 $CODE"

if command -v sqlite3 >/dev/null 2>&1; then
  COUNT=$(sqlite3 "$DATA/app.db" "SELECT COUNT(*) FROM audit_log WHERE action = 'shutdown';" \
    2>/dev/null || echo 0)
  [ "${COUNT:-0}" -ge 1 ] || fail "关闭后应留下 shutdown 审计"
fi

echo "shutdown_exit 通过"
