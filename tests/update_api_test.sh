#!/usr/bin/env bash
# 更新检查接口正反例：验签通过、签名缺失/错误、公钥缺失、中间版本、参数错误。
set -euo pipefail

SERVER_BIN="${1:?用法: update_api_test.sh <xiaotu-server>}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV_PY="$REPO_ROOT/tools/package/.venv/bin/python"
TOOL="$REPO_ROOT/tools/package/update_manifest.py"

if [ ! -x "$VENV_PY" ] || ! "$VENV_PY" -c 'import cryptography' >/dev/null 2>&1; then
  echo "SKIP: 缺少 tools/package/.venv 或 cryptography，跳过需要真实签名的接口测试"
  exit 0
fi

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

head -c 1024 /dev/urandom > "$TMP/app.dmg"
"$VENV_PY" - "$TMP" <<'PY'
import sys
from pathlib import Path
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
root = Path(sys.argv[1])
key = Ed25519PrivateKey.generate()
(root / "key.pem").write_bytes(key.private_bytes(
    serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
(root / "key.pub.pem").write_bytes(key.public_key().public_bytes(
    serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
PY
"$VENV_PY" "$TOOL" generate --file "$TMP/app.dmg" --version 1.4.2 --build 2026111301 \
  --url https://cdn.example.com/a.dmg --schema-min 28 --schema-max 40 \
  --upgrade-step 25:1.3.5:2026040101 --upgrade-step 28:1.3.8:2026100101 \
  --key "$TMP/key.pem" --out "$TMP/latest.json" >/dev/null

mkdir -p "$TMP/data"
cat > "$TMP/config1.toml" <<EOF
[server]
listen = "127.0.0.1"
port = $PORT
[storage]
data_dir = "$TMP/data"
[update]
manifest_file = "$TMP/latest.json"
channel = "stable"
min_supported_version = "1.3.0"
rollout_percent = 100
public_key_file = "$TMP/key.pub.pem"
verifier_python = "$VENV_PY"
verifier_script = "$TOOL"
EOF

"$SERVER_BIN" --server --config "$TMP/config1.toml" > "$TMP/server1.log" 2>&1 &
PID=$!
for _ in $(seq 1 40); do
  curl -s -o /dev/null "http://127.0.0.1:$PORT/api/health" && break
  sleep 0.25
done

base="http://127.0.0.1:$PORT/api/update/check"
query="current_version=1.3.6&build=2026100101&channel=stable&platform=macos&api_version=1&install_id=abc"

ok="$(curl -s "$base?$query&schema_version=30")"
echo "$ok" | grep -q '"update_available":true' || { echo "FAIL: 签名正确应返回可用更新: $ok" >&2; exit 1; }

inter="$(curl -s "$base?$query&schema_version=20")"
echo "$inter" | grep -q '"INTERMEDIATE_REQUIRED"' || { echo "FAIL: 旧 schema 应要求中间版本: $inter" >&2; exit 1; }
echo "$inter" | grep -q '"version":"1.3.5"' || { echo "FAIL: 中间版本应为 1.3.5: $inter" >&2; exit 1; }

status="$(curl -s -o /dev/null -w '%{http_code}' "$base?current_version=1.3.6")"
[ "$status" = "400" ] || { echo "FAIL: 缺参数应返回 400，实际 $status" >&2; exit 1; }

# 篡改清单但不更新签名：服务端必须拒绝。
"$VENV_PY" - "$TMP/latest.json" <<'PY'
import json, sys
path = sys.argv[1]
body = json.load(open(path))
body["build"] = body["build"] + 1
json.dump(body, open(path, "w"), ensure_ascii=False, sort_keys=True)
PY
tampered="$(curl -s "$base?$query&schema_version=30")"
echo "$tampered" | grep -q '"UPDATE_SIGNATURE_INVALID"' || { echo "FAIL: 篡改清单应验签失败: $tampered" >&2; exit 1; }

kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
PID=""

# 未配置公钥：即使清单正确也不能信任。
mkdir -p "$TMP/data2"
"$VENV_PY" "$TOOL" generate --file "$TMP/app.dmg" --version 1.4.2 --build 2026111301 \
  --url https://cdn.example.com/a.dmg --schema-min 28 --schema-max 40 \
  --key "$TMP/key.pem" --out "$TMP/latest.json" >/dev/null
cat > "$TMP/config2.toml" <<EOF
[server]
listen = "127.0.0.1"
port = $((PORT + 1))
[storage]
data_dir = "$TMP/data2"
[update]
manifest_file = "$TMP/latest.json"
channel = "stable"
EOF
"$SERVER_BIN" --server --config "$TMP/config2.toml" > "$TMP/server2.log" 2>&1 &
PID=$!
for _ in $(seq 1 40); do
  curl -s -o /dev/null "http://127.0.0.1:$((PORT + 1))/api/health" && break
  sleep 0.25
done
unverified="$(curl -s "http://127.0.0.1:$((PORT + 1))/api/update/check?$query&schema_version=30")"
echo "$unverified" | grep -q '"UPDATE_UNVERIFIED"' || { echo "FAIL: 缺公钥应拒绝信任: $unverified" >&2; exit 1; }

echo "update_api_test 全部通过"
