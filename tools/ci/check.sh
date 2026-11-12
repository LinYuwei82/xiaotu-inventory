#!/usr/bin/env bash
# 本地与 CI 共用的验证入口：编译、测试、代码生成校验、平台代码泄漏检查。
# 用法：tools/ci/check.sh [--quick]
#   --quick 在 macOS 上跳过 windows-release 构建；Linux 上无额外构建。
# Gitee Go 的 Linux 任务只调用本脚本，不复制检查步骤。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

QUICK=0
if [[ "${1:-}" == "--quick" ]]; then
  QUICK=1
fi

case "$(uname -s)" in
  Darwin)
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
    ;;
  Linux)
    # Gitee Go 云端为 Linux 镜像，没有 macOS 壳与 MSVC；这里验证平台无关部分。
    # Ninja 缺失时降级为 Unix Makefiles，避免把构建工具当成运行环境的前提。
    echo "==> 构建 Linux（XIAOTU_BUILD_SHELL=OFF）"
    GENERATOR=()
    if command -v ninja >/dev/null 2>&1; then
      GENERATOR=(-G Ninja)
    fi
    cmake -S . -B build/ci-linux "${GENERATOR[@]}" \
      -DXIAOTU_BUILD_SHELL=OFF -DCMAKE_BUILD_TYPE=Release
    cmake --build build/ci-linux

    echo "==> 运行 ctest"
    ctest --test-dir build/ci-linux --output-on-failure
    ;;
  *)
    echo "当前平台不受支持：$(uname -s)，请手动执行构建与测试" >&2
    exit 1
    ;;
esac

echo "==> 校验代码生成物"
PYTHON="${XIAOTU_PYTHON:-$ROOT/tools/codegen/.venv/bin/python}"
if [[ ! -x "$PYTHON" ]]; then
  if python3 -m venv "$ROOT/tools/codegen/.venv" 2>/dev/null; then
    "$ROOT/tools/codegen/.venv/bin/pip" install -q -r "$ROOT/tools/codegen/requirements.txt"
    PYTHON="$ROOT/tools/codegen/.venv/bin/python"
  else
    echo "虚拟环境不可用，改用系统 Python" >&2
    python3 -m pip install --user -q -r "$ROOT/tools/codegen/requirements.txt"
    PYTHON=python3
  fi
fi
"$PYTHON" tools/codegen/generate.py --check

echo "==> 检查平台专有代码是否泄漏到 core/server"
"$ROOT/tools/ci/check_platform_leaks.sh"

echo "CI 检查全部通过"
