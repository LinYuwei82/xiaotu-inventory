#!/usr/bin/env bash
# 验证运行中的实例占用数据目录时，--restore 必须以非零退出码失败并说明原因。
# 用法：restore_lock_exit.sh <xiaotu-server 可执行文件>
set -uo pipefail

BIN=${1:?用法: restore_lock_exit.sh <xiaotu-server 可执行文件>}
if [ ! -x "$BIN" ]; then
  echo "FAIL: 找不到可执行文件 $BIN"
  exit 1
fi

DATA=$(mktemp -d "${TMPDIR:-/tmp}/xiaotu-lock-XXXXXX")
PORT=$((20000 + RANDOM % 20000))

"$BIN" --server --port "$PORT" --data "$DATA" >"$DATA/server.out" 2>&1 &
SERVER_PID=$!

cleanup() {
  kill "$SERVER_PID" 2>/dev/null
  wait "$SERVER_PID" 2>/dev/null
  rm -rf "$DATA"
}
trap cleanup EXIT

ready=0
for _ in $(seq 1 100); do
  if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
    ready=1
    break
  fi
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

if [ "$ready" -ne 1 ]; then
  echo "FAIL: 测试服务未能启动"
  cat "$DATA/server.out" 2>/dev/null
  exit 1
fi

OUTPUT=$("$BIN" --restore "$DATA/app.db" --data "$DATA" 2>&1)
CODE=$?

if [ "$CODE" -eq 0 ]; then
  echo "FAIL: 运行中执行 --restore 应返回非零，实际为 0"
  echo "$OUTPUT"
  exit 1
fi
case "$OUTPUT" in
  *"已有小兔进销存实例"*) ;;
  *)
    echo "FAIL: 输出缺少已有实例提示"
    echo "$OUTPUT"
    exit 1
    ;;
esac

echo "restore_lock_exit 通过（退出码 ${CODE}）"
