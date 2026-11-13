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

# ccache 目录显式落在 $HOME 下，便于 Gitee Go 的「构建缓存」按路径挂载。
# CCACHE_BASEDIR 让不同工作区路径之间的缓存可以复用。
if command -v ccache >/dev/null 2>&1; then
  # Linux（含 Gitee Go）固定缓存目录，便于平台按路径挂载；macOS 沿用 ccache 默认目录。
  if [[ "$(uname -s)" == "Linux" ]]; then
    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache}"
    mkdir -p "$CCACHE_DIR"
  fi
  export CCACHE_BASEDIR="$ROOT"
  export CCACHE_COMPILERCHECK=content
  export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-500M}"
fi

print_toolchain() {
  echo "==> 工具链"
  echo "    系统: $(uname -srm)"
  if command -v cmake >/dev/null 2>&1; then
    echo "    cmake: $(cmake --version | head -n 1)"
  else
    echo "    cmake: 缺失（Gitee Go 侧会在缺失或低于 3.25 时安装固定版本，见 README CI 章节）"
    exit 1
  fi
  local tool
  for tool in ninja make ccache gcc g++; do
    if command -v "$tool" >/dev/null 2>&1; then
      echo "    $tool: $(command -v "$tool")"
    else
      echo "    $tool: 缺失"
    fi
  done
}

print_ccache_stats() {
  command -v ccache >/dev/null 2>&1 || return 0
  if [[ -n "${CCACHE_DIR:-}" ]]; then
    echo "    CCACHE_DIR=$CCACHE_DIR"
    # CI 缓存服务通常不展开 ~，填 ~/.cache/ccache 会建出字面名为 ~ 的目录。
    case "$CCACHE_DIR" in
      "~"|"~/"*)
        echo "    警告: CCACHE_DIR 以 ~ 开头，平台缓存不会展开波浪号，实际会落到字面 ~ 目录；" >&2
        echo "          请改用 \$HOME/.cache/ccache 或绝对路径。" >&2
        ;;
    esac
  else
    echo "    CCACHE_DIR=未设置（ccache 使用默认目录）"
  fi
  # 平台若已按字面 ~ 建了目录，这里能直接看出来。
  if [[ -d "$ROOT/~" ]]; then
    echo "    警告: 仓库根目录下存在字面的 ~ 目录，说明缓存路径的波浪号没有被展开。" >&2
  fi
  ccache -s 2>/dev/null | grep -E 'Hits|Misses|hit rate|Cache size' || true
}

print_toolchain

case "$(uname -s)" in
  Darwin)
    echo "==> 构建 release（macOS 全量，含壳）"
    cmake --preset macos-release
    cmake --build --preset release --parallel

    echo "==> 运行 ctest"
    ctest --test-dir build/release --output-on-failure

    # 社区版只保留基础能力，额度与 hidden 能力的拦截路径必须单独构建验证。
    echo "==> 构建 community（能力与额度限制）"
    cmake --preset macos-community
    cmake --build --preset community --parallel
    echo "==> 运行 community ctest"
    ctest --test-dir build/community --output-on-failure

    if [[ "$QUICK" -eq 0 ]]; then
      echo "==> 构建 windows-release（XIAOTU_BUILD_SHELL=OFF）"
      cmake --preset windows-release
      cmake --build --preset windows-release --parallel
      echo "==> 运行 windows-release ctest"
      ctest --test-dir build/windows --output-on-failure
    fi
    ;;
  Linux)
    # Gitee Go 云端为 Linux 镜像，没有 macOS 壳与 MSVC；这里验证平台无关部分。
    # 构建参数统一来自 CMakePresets.json 的 linux-ci preset，不在本脚本里另写一套。
    # Ninja 缺失时用 -G 覆盖 preset 的生成器，避免把构建工具当成运行环境的前提。
    echo "==> 构建 Linux（XIAOTU_BUILD_SHELL=OFF）"
    GENERATOR=()
    GENERATOR_NAME="Ninja"
    if ! command -v ninja >/dev/null 2>&1; then
      GENERATOR=(-G "Unix Makefiles")
      GENERATOR_NAME="Unix Makefiles"
    fi
    echo "    生成器: $GENERATOR_NAME"

    # 构建目录可能来自平台缓存；生成器变化时 CMake 会拒绝复用，先清空重来。
    if [[ -f build/ci-linux/CMakeCache.txt ]]; then
      CACHED_GENERATOR="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' build/ci-linux/CMakeCache.txt)"
      if [[ -n "$CACHED_GENERATOR" && "$CACHED_GENERATOR" != "$GENERATOR_NAME" ]]; then
        echo "    缓存中的生成器为 ${CACHED_GENERATOR}，与当前 ${GENERATOR_NAME} 不一致，清空 build/ci-linux" >&2
        rm -rf build/ci-linux
      fi
    fi

    LAUNCHER=()
    if command -v ccache >/dev/null 2>&1; then
      LAUNCHER=(
        -DCMAKE_C_COMPILER_LAUNCHER=ccache
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
      )
      echo "    ccache: 已启用（${CCACHE_DIR}）"
    fi

    cmake --preset linux-ci "${GENERATOR[@]}" "${LAUNCHER[@]}"
    cmake --build --preset linux-ci --parallel

    echo "==> 运行 ctest"
    ctest --test-dir build/ci-linux --output-on-failure
    ;;
  *)
    echo "当前平台不受支持：$(uname -s)，请手动执行构建与测试" >&2
    exit 1
    ;;
esac

echo "==> ccache 命中情况"
print_ccache_stats

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

echo "==> 检查第三方库本地补丁"
HTTPLIB_PATCH_MARKER="XIAOTU-PATCH:httplib-empty-body-1"
if ! grep -qF "$HTTPLIB_PATCH_MARKER" "$ROOT/third_party/httplib/httplib.h"; then
  echo "httplib 本地补丁缺失：未找到标记 $HTTPLIB_PATCH_MARKER" >&2
  echo "升级 httplib 后必须按 docs/第三方补丁.md 逐条复核并恢复补丁。" >&2
  exit 1
fi
echo "    httplib 补丁标记存在（见 docs/第三方补丁.md）"

echo "==> 检查前端脚本语法（jsc）"
JSC_BIN=""
if command -v jsc >/dev/null 2>&1; then
  JSC_BIN="$(command -v jsc)"
elif [ -x "/System/Library/Frameworks/JavaScriptCore.framework/Versions/A/Helpers/jsc" ]; then
  JSC_BIN="/System/Library/Frameworks/JavaScriptCore.framework/Versions/A/Helpers/jsc"
fi
if [ -z "$JSC_BIN" ]; then
  echo "    跳过：未找到 jsc，web/app.js 未做语法机检（Linux CI 预期如此）" >&2
else
  if ! "$JSC_BIN" -e "try { new Function(readFile('$ROOT/web/app.js')); print('    web/app.js 语法通过'); } catch (e) { print('web/app.js 语法错误: ' + e); quit(1); }"; then
    exit 1
  fi
fi

echo "==> 检查平台专有代码是否泄漏到 core/server"
"$ROOT/tools/ci/check_platform_leaks.sh"

echo "CI 检查全部通过"
