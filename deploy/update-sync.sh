#!/usr/bin/env bash
# 从厂商更新源同步静态清单到本机（网络版/私有化部署使用）。
#
# 只同步清单与 appcast，不同步安装包；安装包由客户端直接访问清单中的 url。
# 同步采用临时文件 + 原子替换，避免服务端读到半个文件。
#
# 环境变量：
#   XIAOTU_UPDATE_SOURCE          厂商清单 URL（必填，https）
#   XIAOTU_UPDATE_TARGET          本地清单落盘路径（必填）
#   XIAOTU_UPDATE_APPCAST_SOURCE  可选，厂商 appcast URL
#   XIAOTU_UPDATE_APPCAST_TARGET  可选，appcast 落盘路径（与 SOURCE 同时提供）
#   XIAOTU_UPDATE_PUBKEY          可选，Ed25519 公钥 PEM；提供时调用 Python 工具验签清单
#   XIAOTU_UPDATE_PYTHON          可选，Python 解释器（默认 python3）
#   XIAOTU_UPDATE_TOOL            可选，清单验签工具路径
#                                 默认：tools/release/manifest_v2.py（纯标准库，无额外依赖）
#                                 也可用 tools/package/update_manifest.py（依赖 cryptography，同时支持 v1 latest.json）
#                                 两者 verify 接口一致：verify --manifest <文件> --pub <公钥 PEM>

set -euo pipefail

: "${XIAOTU_UPDATE_SOURCE:?需要 XIAOTU_UPDATE_SOURCE}"
: "${XIAOTU_UPDATE_TARGET:?需要 XIAOTU_UPDATE_TARGET}"

TARGET_DIR="$(dirname "$XIAOTU_UPDATE_TARGET")"
mkdir -p "$TARGET_DIR"
TMP="$(mktemp "${TARGET_DIR}/.manifest.XXXXXX")"
trap 'rm -f "$TMP"' EXIT

curl --fail --silent --show-error --location \
  --connect-timeout 5 --max-time 20 \
  --proto '=https' \
  --output "$TMP" \
  "$XIAOTU_UPDATE_SOURCE"

if [ -n "${XIAOTU_UPDATE_PUBKEY:-}" ]; then
  PYTHON_BIN="${XIAOTU_UPDATE_PYTHON:-python3}"
  TOOL="${XIAOTU_UPDATE_TOOL:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/tools/release/manifest_v2.py}"
  "$PYTHON_BIN" "$TOOL" verify --manifest "$TMP" --pub "$XIAOTU_UPDATE_PUBKEY" >/dev/null
fi

chmod 0644 "$TMP"
mv -f "$TMP" "$XIAOTU_UPDATE_TARGET"
echo "已同步更新清单：$XIAOTU_UPDATE_TARGET"

# 可选：同步 appcast（Sparkle 客户端使用）；同样先写临时文件再原子替换。
if [ -n "${XIAOTU_UPDATE_APPCAST_SOURCE:-}" ] && [ -n "${XIAOTU_UPDATE_APPCAST_TARGET:-}" ]; then
  APPCAST_DIR="$(dirname "$XIAOTU_UPDATE_APPCAST_TARGET")"
  mkdir -p "$APPCAST_DIR"
  APPCAST_TMP="$(mktemp "${APPCAST_DIR}/.appcast.XXXXXX")"
  curl --fail --silent --show-error --location \
    --connect-timeout 5 --max-time 20 \
    --proto '=https' \
    --output "$APPCAST_TMP" \
    "$XIAOTU_UPDATE_APPCAST_SOURCE"
  chmod 0644 "$APPCAST_TMP"
  mv -f "$APPCAST_TMP" "$XIAOTU_UPDATE_APPCAST_TARGET"
  echo "已同步 appcast：$XIAOTU_UPDATE_APPCAST_TARGET"
fi
