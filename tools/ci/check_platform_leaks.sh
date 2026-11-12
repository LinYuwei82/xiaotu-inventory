#!/usr/bin/env bash
# 平台代码泄漏检查：
#   1. core/ 不得出现 Apple 专有头、框架或条件编译；
#      唯一例外是 core/include/xiaotu/platform.h：它只补齐标准库在 POSIX 与
#      Windows 上的能力差异（local_time / path_to_utf8），不引入平台框架与句柄，
#      新增文件若需要条件编译，应先说服这条规则，而不是绕过检查；
#   2. server/ 只允许 cli_main.cpp 在 #if defined(__APPLE__) 内引用 WebView 壳；
#   3. shell/ 本身是 macOS 专有目录，不参与检查。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

fail=0

check() {
  local description="$1"
  shift
  local output
  if output="$("$@" 2>/dev/null)"; then
    if [[ -n "$output" ]]; then
      echo "平台代码泄漏：$description" >&2
      echo "$output" >&2
      fail=1
    fi
  fi
}

# core/：不允许任何 Apple 专有条件编译、框架或 WebView 引用。
check "core/ 出现 __APPLE__" \
  grep -REn --include='*.h' --include='*.cpp' --include='*.mm' '__APPLE__' core
check "core/ 引用 Apple 框架或 WebView" \
  grep -REn --include='*.h' --include='*.cpp' --include='*.mm' \
  'webview\.h|CoreFoundation|CoreGraphics|AppKit|WebKit|WKWebView' core

# server/：只有 cli_main.cpp 允许受 __APPLE__ 保护的 WebView 壳引用。
check "server/ 引用 Apple 框架或 WebView（cli_main.cpp 除外）" \
  bash -c "grep -REn --include='*.h' --include='*.cpp' --include='*.mm' \
    'webview\\.h|CoreFoundation|CoreGraphics|AppKit|WebKit|WKWebView' server \
    | grep -v 'server/src/cli_main.cpp' || true"

if [[ "$fail" -ne 0 ]]; then
  echo "平台代码泄漏检查未通过" >&2
  exit 1
fi
echo "平台代码泄漏检查通过"
