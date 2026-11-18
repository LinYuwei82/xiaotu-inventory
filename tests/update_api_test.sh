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

# 轻量握手：health 必须回显服务端要求的最低客户端版本，供桌面壳阻断旧版本。
health="$(curl -s "http://127.0.0.1:$PORT/api/health")"
echo "$health" | grep -q '"min_supported_version":"1.3.0"' || {
  echo "FAIL: health 应返回 min_supported_version: $health" >&2; exit 1;
}

# 低于最低支持版本时，即使清单判定为无更新也必须有强制标记。
required="$(curl -s "$base?current_version=1.2.0&build=2026100101&channel=stable&platform=macos&api_version=1&install_id=abc&schema_version=30")"
echo "$required" | grep -q '"mandatory":true' || { echo "FAIL: 低于最低支持版本应强制升级: $required" >&2; exit 1; }
echo "$required" | grep -q '"MIN_SUPPORTED"' || { echo "FAIL: 强制原因应为 MIN_SUPPORTED: $required" >&2; exit 1; }

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

# 新增接口沿用会话鉴权：未登录访问状态接口必须 401，不能像 /api/update/check 那样免登录。
status_code="$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$((PORT + 1))/api/update/status")"
[ "$status_code" = "401" ] || { echo "FAIL: 未登录访问 /api/update/status 应返回 401，实际 $status_code" >&2; exit 1; }

kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
PID=""

# 未配置 [update]：检查接口必须明确报未配置，而不是静默返回“无更新”。
mkdir -p "$TMP/data3"
cat > "$TMP/config3.toml" <<EOF
[server]
listen = "127.0.0.1"
port = $((PORT + 2))
[storage]
data_dir = "$TMP/data3"
EOF
"$SERVER_BIN" --server --config "$TMP/config3.toml" > "$TMP/server3.log" 2>&1 &
PID=$!
for _ in $(seq 1 40); do
  curl -s -o /dev/null "http://127.0.0.1:$((PORT + 2))/api/health" && break
  sleep 0.25
done
not_configured="$(curl -s -w '\n%{http_code}' "http://127.0.0.1:$((PORT + 2))/api/update/check?$query&schema_version=30")"
echo "$not_configured" | grep -q '"UPDATE_NOT_CONFIGURED"' || {
  echo "FAIL: 未配置更新源应返回 UPDATE_NOT_CONFIGURED: $not_configured" >&2; exit 1;
}
echo "$not_configured" | grep -q '未配置更新清单' || {
  echo "FAIL: 未配置更新源应给出明确文案: $not_configured" >&2; exit 1;
}

kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
PID=""

# 登录后查看更新状态：单机版用一次性令牌换会话，验证状态接口的鉴权与展示字段。
mkdir -p "$TMP/data4"
"$SERVER_BIN" --data "$TMP/data4" > "$TMP/server4.log" 2>&1 &
PID=$!
LOCAL_PORT=""
for _ in $(seq 1 60); do
  LOCAL_PORT="$(sed -n 's|.*服务地址: http://127.0.0.1:\([0-9]*\).*|\1|p' "$TMP/server4.log" | head -1)"
  [ -n "$LOCAL_PORT" ] && break
  sleep 0.25
done
[ -n "$LOCAL_PORT" ] || { echo "FAIL: 单机版未能启动" >&2; cat "$TMP/server4.log" >&2; exit 1; }
BOOTSTRAP="$(sed -n 's|.*#t=\([0-9a-f]*\).*|\1|p' "$TMP/server4.log" | head -1)"
[ -n "$BOOTSTRAP" ] || { echo "FAIL: 未找到单机访问令牌" >&2; exit 1; }
csrf="$(curl -s -c "$TMP/cookies" -X POST -H 'Content-Type: application/json' \
  -d "{\"token\":\"$BOOTSTRAP\"}" "http://127.0.0.1:$LOCAL_PORT/api/session" |
  sed -n 's|.*"csrf":"\([^"]*\)".*|\1|p')"
[ -n "$csrf" ] || { echo "FAIL: 单机令牌换会话失败" >&2; exit 1; }
status_json="$(curl -s -b "$TMP/cookies" "http://127.0.0.1:$LOCAL_PORT/api/update/status")"
echo "$status_json" | grep -q '"configured":false' || {
  echo "FAIL: 状态接口应报告未配置更新源: $status_json" >&2; exit 1;
}
echo "$status_json" | grep -q '未配置更新清单' || {
  echo "FAIL: 状态接口应带未配置原因: $status_json" >&2; exit 1;
}
echo "$status_json" | grep -q '"current":' || {
  echo "FAIL: 状态接口应返回当前版本: $status_json" >&2; exit 1;
}
echo "$status_json" | grep -q '"version":"' || {
  echo "FAIL: 状态接口应返回当前版本号: $status_json" >&2; exit 1;
}
echo "$status_json" | grep -q '"build":' || {
  echo "FAIL: 状态接口应返回构建号: $status_json" >&2; exit 1;
}

# v2 统一清单：线上实际格式，验证 HTTP 层按平台拍平、缺平台与验签失败。
MANIFEST_V2="$REPO_ROOT/tools/release/manifest_v2.py"
cp "$TMP/app.dmg" "$TMP/xiaotu-1.4.2-macos-arm64.dmg"
"$VENV_PY" "$MANIFEST_V2" generate --product xiaotu --platform macos \
  --version 1.4.2 --build 0 --artifact "dmg=$TMP/xiaotu-1.4.2-macos-arm64.dmg" \
  --key "$TMP/key.pem" --out "$TMP/manifest.json" >/dev/null

mkdir -p "$TMP/data5"
cat > "$TMP/config5.toml" <<EOF
[server]
listen = "127.0.0.1"
port = $((PORT + 3))
[storage]
data_dir = "$TMP/data5"
[update]
manifest_file = "$TMP/manifest.json"
channel = "stable"
public_key_file = "$TMP/key.pub.pem"
verifier_python = "$VENV_PY"
verifier_script = "$MANIFEST_V2"
EOF

"$SERVER_BIN" --server --config "$TMP/config5.toml" > "$TMP/server5.log" 2>&1 &
PID=$!
for _ in $(seq 1 40); do
  curl -s -o /dev/null "http://127.0.0.1:$((PORT + 3))/api/health" && break
  sleep 0.25
done

v2base="http://127.0.0.1:$((PORT + 3))/api/update/check"
v2ok="$(curl -s "$v2base?$query&schema_version=30")"
echo "$v2ok" | grep -q '"update_available":true' || { echo "FAIL: v2 清单应判定可用更新: $v2ok" >&2; exit 1; }
echo "$v2ok" | grep -q '"version":"1.4.2"' || { echo "FAIL: v2 版本应为 1.4.2: $v2ok" >&2; exit 1; }

# 请求平台在清单里不存在：必须明确报错，不能回落到另一平台。
v2miss="$(curl -s "$v2base?current_version=1.3.6&build=2026100101&channel=stable&platform=windows&api_version=1&install_id=abc&schema_version=30")"
echo "$v2miss" | grep -q '"MANIFEST_PLATFORM_MISSING"' || { echo "FAIL: v2 缺平台段应报错: $v2miss" >&2; exit 1; }

# 篡改 v2 清单同样必须验签失败。
"$VENV_PY" - "$TMP/manifest.json" <<'PY'
import json, sys
path = sys.argv[1]
body = json.load(open(path))
body["platforms"]["macos"]["version"] = "9.9.9"
json.dump(body, open(path, "w"), ensure_ascii=False, sort_keys=True)
PY
v2tampered="$(curl -s "$v2base?$query&schema_version=30")"
echo "$v2tampered" | grep -q '"UPDATE_SIGNATURE_INVALID"' || { echo "FAIL: 篡改 v2 清单应验签失败: $v2tampered" >&2; exit 1; }

kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
PID=""

echo "update_api_test 全部通过"
