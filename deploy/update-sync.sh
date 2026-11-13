#!/usr/bin/env bash
# 从厂商更新源同步静态清单到本机（网络版/私有化部署使用）。
#
# 只同步清单与 appcast，不同步安装包；安装包由客户端直接访问清单中的 url。
# 同步采用临时文件 + 原子替换，避免服务端读到半个文件。
#
# 环境变量：
#   XIAOTU_UPDATE_SOURCE    厂商清单 URL（必填，https）
#   XIAOTU_UPDATE_TARGET    本地落盘路径（必填，如 /srv/xiaotu-updates/macos/stable/latest.json）
#   XIAOTU_UPDATE_PUBKEY    可选，Ed25519 公钥 PEM；提供时调用 Python 工具验签
#   XIAOTU_UPDATE_PYTHON    可选，Python 解释器（默认 python3）
#   XIAOTU_UPDATE_TOOL      可选，update_manifest.py 路径

set -euo pipefail

: "${XIAOTU_UPDATE_SOURCE:?需要 XIAOTU_UPDATE_SOURCE}"
: "${XIAOTU_UPDATE_TARGET:?需要 XIAOTU_UPDATE_TARGET}"

TARGET_DIR="$(dirname "$XIAOTU_UPDATE_TARGET")"
mkdir -p "$TARGET_DIR"
TMP="$(mktemp "${TARGET_DIR}/.latest.XXXXXX")"
trap 'rm -f "$TMP"' EXIT

curl --fail --silent --show-error --location \
  --connect-timeout 5 --max-time 20 \
  --proto '=https' \
  --output "$TMP" \
  "$XIAOTU_UPDATE_SOURCE"

if [ -n "${XIAOTU_UPDATE_PUBKEY:-}" ]; then
  PYTHON_BIN="${XIAOTU_UPDATE_PYTHON:-python3}"
  TOOL="${XIAOTU_UPDATE_TOOL:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/tools/package/update_manifest.py}"
  "$PYTHON_BIN" "$TOOL" verify --manifest "$TMP" --pub "$XIAOTU_UPDATE_PUBKEY" >/dev/null
fi

chmod 0644 "$TMP"
mv -f "$TMP" "$XIAOTU_UPDATE_TARGET"
echo "已同步更新清单：$XIAOTU_UPDATE_TARGET"
