#!/usr/bin/env bash
# 小兔进销存 · 夜间任务（只在“家里”那台 macmini 上运行）
#
# 内容：全量构建 → ctest → ASan/UBSan 构建与测试 → 静态分析 → 汇总
# 日志：按日期落盘，默认 ~/Library/Logs/xiaotu-nightly/nightly-YYYY-MM-DD.log
# 通知：设置 XIAOTU_NIGHTLY_WEBHOOK 后，失败时 POST 一段 JSON；未设置则只写日志
#
# 用法：
#   tools/nightly/nightly.sh          # 完整执行
#   tools/nightly/nightly.sh --help   # 查看可用环境变量
#
# 依赖：cmake 3.25+、ninja、ccache、ctest；静态分析用 clang-tidy，缺失时回落到 cppcheck。
# 构建参数与 CMakePresets.json 的 macos-dev / macos-release 保持一致，不引入其他构建体系。

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT" || exit 1

LOG_DIR="${XIAOTU_LOG_DIR:-$HOME/Library/Logs/xiaotu-nightly}"
ASAN_DIR="${XIAOTU_ASAN_DIR:-$REPO_ROOT/build/asan}"
TIDY_DIR="${XIAOTU_TIDY_DIR:-$REPO_ROOT/build/tidy}"
SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
SAN_LINK_FLAGS="-fsanitize=address,undefined"

usage() {
  cat <<'EOF'
小兔进销存夜间任务

环境变量：
  XIAOTU_LOG_DIR          日志目录（默认 ~/Library/Logs/xiaotu-nightly）
  XIAOTU_ASAN_DIR         ASan/UBSan 构建目录（默认 <repo>/build/asan）
  XIAOTU_TIDY_DIR         静态分析构建目录（默认 <repo>/build/tidy）
  XIAOTU_NIGHTLY_WEBHOOK  失败通知地址（POST JSON）；未设置则只写日志
  XIAOTU_NIGHTLY_JOBS     并行编译数（默认交给 Ninja 决定）

退出码：全部通过为 0，任一步失败为 1。
EOF
}

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  "") ;;
  *) echo "未知参数：$1（用 --help 查看用法）" >&2; exit 2 ;;
esac

mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/nightly-$(date '+%Y-%m-%d').log"

# 同一时间只跑一份，避免两轮任务互相覆盖 build/ 与日志
LOCK_DIR="$LOG_DIR/.lock"
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
  echo "已有夜间任务在运行（$LOCK_DIR 存在）；若上次异常退出，手动删除该目录后重试。" >&2
  exit 0
fi
trap 'rmdir "$LOCK_DIR" 2>/dev/null' EXIT

exec > >(tee -a "$LOG_FILE") 2>&1

echo "===== 夜间任务开始：$(date '+%Y-%m-%d %H:%M:%S') ====="
echo "仓库：$REPO_ROOT"
echo "日志：$LOG_FILE"
echo "分支：$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo '未知')  提交：$(git rev-parse --short HEAD 2>/dev/null || echo '未知')"

FAILED=()

run_step() {
  local name="$1"
  shift
  echo
  echo "===== [$(date '+%H:%M:%S')] $name ====="
  if "$@"; then
    echo "----- ${name}：通过"
  else
    local rc=$?
    echo "----- ${name}：失败（exit ${rc}）"
    FAILED+=("$name")
  fi
}

step_build() {
  cmake --preset macos-release && cmake --build --preset release
}

step_ctest() {
  ctest --test-dir build/release --output-on-failure
}

step_asan_build() {
  cmake -S "$REPO_ROOT" -B "$ASAN_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DXIAOTU_DEV_WEB=ON \
    -DCMAKE_C_FLAGS="$SAN_FLAGS" \
    -DCMAKE_CXX_FLAGS="$SAN_FLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$SAN_LINK_FLAGS" || return 1
  cmake --build "$ASAN_DIR"
}

step_asan_ctest() {
  ctest --test-dir "$ASAN_DIR" --output-on-failure
}

step_analyze() {
  cmake -S "$REPO_ROOT" -B "$TIDY_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DXIAOTU_DEV_WEB=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON || return 1

  if command -v clang-tidy >/dev/null 2>&1; then
    echo "静态分析：clang-tidy"
    while IFS= read -r file; do
      echo "-- $file"
      clang-tidy -p "$TIDY_DIR" --quiet "$file" || return 1
    done < <(find core server shell tests -type f -name '*.cpp' -not -path '*/build/*' | sort)
    return 0
  fi

  if command -v cppcheck >/dev/null 2>&1; then
    echo "静态分析：cppcheck（未找到 clang-tidy）"
    cppcheck --std=c++20 --enable=warning,performance,portability \
      --inline-suppr --error-exitcode=1 --quiet \
      -I core/include -I server/include -I third_party \
      core/src server/src shell/main.cpp shell/common tests
    return $?
  fi

  echo "静态分析：跳过（clang-tidy 与 cppcheck 均未安装）"
  return 0
}

run_step "构建（macos-release）" step_build
run_step "ctest（build/release）" step_ctest
run_step "ASan/UBSan 构建（build/asan）" step_asan_build
run_step "ctest（ASan/UBSan）" step_asan_ctest
run_step "静态分析" step_analyze

FINISHED_AT="$(date '+%Y-%m-%d %H:%M:%S')"
echo
echo "===== 夜间任务结束：$FINISHED_AT ====="

if [ ${#FAILED[@]} -eq 0 ]; then
  echo "结果：全部通过"
  STATUS="ok"
  FAILED_TEXT=""
else
  FAILED_TEXT="$(printf '%s, ' "${FAILED[@]}")"
  FAILED_TEXT="${FAILED_TEXT%, }"
  echo "结果：失败（${FAILED_TEXT}）"
  STATUS="failed"
fi

notify() {
  local webhook="${XIAOTU_NIGHTLY_WEBHOOK:-}"
  if [ -z "$webhook" ]; then
    echo "未配置 XIAOTU_NIGHTLY_WEBHOOK，仅写日志。"
    return 0
  fi

  local payload="$LOG_DIR/.webhook-$$.json"
  {
    printf '{"service":"xiaotu-inventory","host":"%s","status":"%s",' "$(hostname -s)" "$STATUS"
    printf '"failed_steps":"%s","log":"%s","finished_at":"%s"}' "$FAILED_TEXT" "$LOG_FILE" "$FINISHED_AT"
  } > "$payload"

  echo "发送通知：$webhook"
  if curl -fsS --max-time 10 -H 'Content-Type: application/json' \
      --data-binary @"$payload" "$webhook" >/dev/null; then
    echo "通知已发送。"
  else
    echo "通知发送失败（不影响任务退出码）。"
  fi
  rm -f "$payload"
}

if [ "$STATUS" != "ok" ]; then
  notify
fi

[ "$STATUS" = "ok" ] && exit 0 || exit 1
