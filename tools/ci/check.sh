#!/usr/bin/env bash
# 本地与 CI 共用的验证入口：编译、测试、代码生成校验、平台代码泄漏检查。
# 用法：tools/ci/check.sh [--quick]
#   --quick 只跑本机 release 构建与测试，不构建 windows-release。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

QUICK=0
if [[ "${1:-}" == "--quick" ]]; then
  QUICK=1
fi

echo "==> 构建 release（macOS 全量，含壳）"
cmake --preset macos-release
cmake --build --preset release

echo "==> 运行 ctest"
ctest --test-dir build/release --output-on-failure

if [[ "$QUICK" -eq 0 ]]; then
  echo "==> 构建 windows-release（XIAOTU_BUILD_SHELL=OFF）"
  cmake --preset windows-release
  cmake --build --preset windows-release
  echo "==> 运行 windows-release ctest"
  ctest --test-dir build/windows --output-on-failure
fi

echo "==> 校验代码生成物"
PYTHON="${XIAOTU_PYTHON:-$ROOT/tools/codegen/.venv/bin/python}"
if [[ ! -x "$PYTHON" ]]; then
  echo "缺少代码生成虚拟环境：$PYTHON" >&2
  echo "请先执行 python3 -m venv tools/codegen/.venv && tools/codegen/.venv/bin/pip install -r tools/codegen/requirements.txt" >&2
  exit 1
fi
"$PYTHON" tools/codegen/generate.py --check

echo "==> 检查平台专有代码是否泄漏到 core/server"
"$ROOT/tools/ci/check_platform_leaks.sh"

echo "CI 检查全部通过"
