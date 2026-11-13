#!/usr/bin/env bash
# 更新清单工具的签名/规范化/篡改检测测试；未安装 cryptography 时明确跳过。
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON_BIN="${PYTHON:-${REPO_ROOT}/tools/package/.venv/bin/python}"

if [ ! -x "$PYTHON_BIN" ]; then
  echo "SKIP: 未找到 $PYTHON_BIN（可创建 tools/package/.venv 并安装 requirements.txt）"
  exit 0
fi
if ! "$PYTHON_BIN" -c 'import cryptography' >/dev/null 2>&1; then
  echo "SKIP: $PYTHON_BIN 未安装 cryptography"
  exit 0
fi

cd "$REPO_ROOT"
"$PYTHON_BIN" -m unittest discover -s tools/package -p 'test_*.py' -v
