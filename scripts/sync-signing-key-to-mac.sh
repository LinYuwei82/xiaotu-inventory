#!/usr/bin/env bash
# sync-signing-key-to-mac.sh
#
# 从 Windows 把 Ed25519 更新签名密钥同步到本机 Mac。
# 信任锚公钥已硬编码进仓库 CMakeLists.txt 两处，私钥只在两端本地落盘，
# 永远不进 git、不贴到任何云端。
#
# 用法：
#   1. 在 Mac 上打开终端，cd 到本仓库根
#   2. 确认 Windows 机器在线（ping <Windows-IP> 通）
#   3. ./scripts/sync-signing-key-to-mac.sh
#
# 脚本会自动：备份旧密钥 → 拷新密钥 → 验证同源（公钥 base64 必须匹配）
#              → 输出 publish_oss.sh 调用示例

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KEY_DIR="$HOME/.config/xiaotu-updates"
WIN_USER="Lin"

# === 1. 密钥目录 & 备份 ===
mkdir -p "$KEY_DIR"
chmod 700 "$KEY_DIR"

if [ -f "$KEY_DIR/update_signing_2026.pem" ]; then
  BACKUP="$KEY_DIR/update_signing_2026.pem.bak.$(date +%Y%m%d%H%M%S)"
  mv "$KEY_DIR/update_signing_2026.pem" "$BACKUP"
  mv "$KEY_DIR/update_signing_2026.pub.pem" "${BACKUP}.pub" 2>/dev/null || true
  echo "[备份] 旧私钥 → $BACKUP"
fi

# === 2. 确定 Windows 地址 ===
WIN_IP="${1:-}"
if [ -z "$WIN_IP" ]; then
  echo "Windows IP 未传，尝试从仓库根 .env.local 读…"
  WIN_IP="$(grep -E '^WINDOWS_IP=' "$REPO_ROOT/.env.local" 2>/dev/null | cut -d= -f2 | tr -d '\r' || true)"
fi
if [ -z "$WIN_IP" ]; then
  echo ""
  echo "Windows IP 未找到。请用下面任选一种方式："
  echo "  A.  把脚本改成接受参数后：./scripts/sync-signing-key-to-mac.sh 192.168.50.130"
  echo "  B.  手动拖文件：Finder 连 Windows → C:\\Users\\Lin\\.config\\xiaotu-updates\\"
  echo "      拷 update_signing_2026.pem 和 .pub.pem 到 $KEY_DIR"
  echo "      然后 chmod 600 $KEY_DIR/update_signing_2026.pem 再重新跑本脚本"
  echo "  C.  手动 scp："
  echo "      scp ${WIN_USER}@<Windows-IP>:C:/Users/Lin/.config/xiaotu-updates/update_signing_2026.pem $KEY_DIR/"
  echo "      scp ${WIN_USER}@<Windows-IP>:C:/Users/Lin/.config/xiaotu-updates/update_signing_2026.pub.pem $KEY_DIR/"
  echo ""
  echo "退出（未做任何破坏性操作）。"
  exit 1
fi

# === 3. 拉密钥 ===
echo "[拉取] scp ${WIN_USER}@${WIN_IP} → $KEY_DIR/"
if ! scp "${WIN_USER}@${WIN_IP}:C:/Users/Lin/.config/xiaotu-updates/update_signing_2026.pem" "$KEY_DIR/" 2>&1; then
  echo "scp 失败。可能需要密码（请手动在 Finder 拖或输 sshpass）。退出。"
  exit 1
fi
if ! scp "${WIN_USER}@${WIN_IP}:C:/Users/Lin/.config/xiaotu-updates/update_signing_2026.pub.pem" "$KEY_DIR/" 2>&1; then
  echo "公钥没拉到，退出。"
  exit 1
fi
chmod 600 "$KEY_DIR/update_signing_2026.pem"

# === 4. 验证同源 ===
echo "[验同源] Mac 生成的公钥 base64："
MAC_BASE64="$(openssl pkey -pubin -in "$KEY_DIR/update_signing_2026.pub.pem" \
  -outform DER 2>/dev/null | tail -c 32 | base64 | tr -d '\n')"
echo "  $MAC_BASE64"

EXPECTED_BASE64="HgCDvW0MQ32HFguQ/jW5fVtQh74lBsXMsyfo0sRLH1Y="
echo "期望（CMake 硬编码）："
echo "  $EXPECTED_BASE64"

if [ "$MAC_BASE64" != "$EXPECTED_BASE64" ]; then
  echo ""
  echo "❌ 公钥不一致！Mac 上这把私钥不是 Windows 那把。"
  echo "   Windows 壳里 trust anchor 验签会拒所有清单，必须同源。"
  echo "   回去确认 Windows 上 .config/xiaotu-updates/update_signing_2026.pem 是不是刚才那把。"
  exit 2
fi

echo "✅ 公钥同源，trust anchor 一致。"

# === 5. 给 Mac 默认路径 ===
cat <<EOF

=== 同步完成 ===

私钥：$KEY_DIR/update_signing_2026.pem  （chmod 600，仅本机可读写）
公钥：$KEY_DIR/update_signing_2026.pub.pem （publish_oss.sh / verify_manifest 用）
公钥 base64：$MAC_BASE64

publish_oss.sh 默认就找 ~/.config/xiaotu-updates/update_signing_2026.pem，
直接跑不用设环境变量：

  PRODUCT=xiaotu VERSION=1.0.0 BUILD=\$(date +%Y%m%d%H) CHANNEL=stable \\
    tools/package/publish_oss.sh

验证清单验签本地可用：

  python3 tools/package/update_manifest.py verify \\
    --manifest /tmp/latest.json \\
    --pub "$KEY_DIR/update_signing_2026.pub.pem" \\
    --file /tmp/xiaotu-inventory-1.0.0.dmg

EOF
