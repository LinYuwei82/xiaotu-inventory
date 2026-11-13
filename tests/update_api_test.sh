#!/usr/bin/env bash
# 更新检查接口的正反例：无需登录、缺参数 400、未配置更新源返回无更新。
set -euo pipefail

SERVER_BIN="${1:?用法: update_api_test.sh <xiaotu-server>}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/xiaotu-update-api.XXXXXX")"
PORT=18099
PID=""

cleanup() {
  if [ -n "$PID" ]; then
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT

mkdir -p "$TMP/data"
cat > "$TMP/manifest.json" <<'JSON'
{"manifest_version":1,"key_id":"2026-01","platform":"macos","channel":"stable","version":"1.4.2","build":2026111301,"min_os":"12.0","url":"https://cdn.example.com/a.dmg","sha256":"9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c","size":1024,"published_at":"2026-11-13T08:00:00Z","release_notes_url":"https://example.com/n","mandatory":false,"schema_min":28,"schema_max":40,"api_min":1,"api_max":2,"rollout_percent":100,"signature":"c2ln"}
JSON
cat > "$TMP/config.toml" <<EOF
[server]
listen = "127.0.0.1"
port = $PORT
[storage]
data_dir = "$TMP/data"
[update]
manifest_file = "$TMP/manifest.json"
channel = "stable"
min_supported_version = "1.3.0"
rollout_percent = 100
EOF

"$SERVER_BIN" --server --config "$TMP/config.toml" > "$TMP/server.log" 2>&1 &
PID=$!
for _ in $(seq 1 40); do
  if curl -s -o /dev/null "http://127.0.0.1:$PORT/api/health"; then
    break
  fi
  sleep 0.25
done

base="http://127.0.0.1:$PORT/api/update/check"
ok="$(curl -s "$base?current_version=1.2.0&build=2026100101&channel=stable&platform=macos&schema_version=30&api_version=1&install_id=abc")"
echo "$ok" | grep -q '"update_available":true' || { echo "FAIL: 应返回可用更新: $ok" >&2; exit 1; }
echo "$ok" | grep -q '"mandatory":true' || { echo "FAIL: 应返回强制更新: $ok" >&2; exit 1; }
echo "$ok" | grep -q '"MIN_SUPPORTED"' || { echo "FAIL: 强制原因应为 MIN_SUPPORTED: $ok" >&2; exit 1; }

status="$(curl -s -o /dev/null -w '%{http_code}' "$base?current_version=1.2.0")"
[ "$status" = "400" ] || { echo "FAIL: 缺参数应返回 400，实际 $status" >&2; exit 1; }

cat > "$TMP/config2.toml" <<EOF
[server]
listen = "127.0.0.1"
port = $((PORT + 1))
[storage]
data_dir = "$TMP/data2"
EOF
mkdir -p "$TMP/data2"
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
PID=""
"$SERVER_BIN" --server --config "$TMP/config2.toml" > "$TMP/server2.log" 2>&1 &
PID=$!
for _ in $(seq 1 40); do
  if curl -s -o /dev/null "http://127.0.0.1:$((PORT + 1))/api/health"; then
    break
  fi
  sleep 0.25
done
unconfigured="$(curl -s "http://127.0.0.1:$((PORT + 1))/api/update/check?current_version=1.2.0&build=1&channel=stable&platform=macos&schema_version=30")"
echo "$unconfigured" | grep -q '"update_available":false' || { echo "FAIL: 未配置应无更新: $unconfigured" >&2; exit 1; }
echo "$unconfigured" | grep -q '"UPDATE_NOT_CONFIGURED"' || { echo "FAIL: 未配置应返回 UPDATE_NOT_CONFIGURED: $unconfigured" >&2; exit 1; }

echo "update_api_test 全部通过"
