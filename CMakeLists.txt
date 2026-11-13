cmake_minimum_required(VERSION 3.25)

# 产品版本的唯一来源：仓库根 VERSION 文件。macOS 壳、Windows 壳与二进制
# 都从这里取值，避免 CMakeLists、Info.plist、资源文件各写一份。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" XIAOTU_VERSION_STRING)
string(STRIP "${XIAOTU_VERSION_STRING}" XIAOTU_VERSION_STRING)
project(xiaotu_inventory VERSION ${XIAOTU_VERSION_STRING} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC 默认按系统代码页（中文 Windows 为 936）解析源码，而仓库源码是无 BOM 的 UTF-8，
# 会把中文字面量读坏并触发 C4819 / 语法错误。GCC 与 Clang 默认即 UTF-8，不需要该选项。
if(MSVC)
  add_compile_options(/utf-8)
endif()

if(APPLE)
  enable_language(OBJCXX)
  if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "macOS 最低版本")
  endif()
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "构建类型" FORCE)
endif()

# 版本：full（全功能版，默认）或 community（社区版）。
# 能力与额度取自 tools/codegen/schema/editions.json，这里只做选择，不写死数值。
set(XIAOTU_EDITION "full" CACHE STRING "版本：full（全功能版）或 community（社区版）")
message(STATUS "版本：${XIAOTU_EDITION}")

# 构建号：单调递增，发布时由流水线显式传入；本地默认值只用于开发。
set(XIAOTU_BUILD "2026111301" CACHE STRING "构建号（单调递增）")
# API 版本：破坏性协议变更时 +1，并保留 N-1 一个 MINOR 周期。
set(XIAOTU_API_VERSION "1" CACHE STRING "API 协议版本")

# 单机版壳（WKWebView + .app）默认构建；Windows / 无头开发服务器时关闭，
# 只构建 core、server 与测试，用浏览器访问 --server。
option(XIAOTU_BUILD_SHELL "构建 macOS 单机版 WebView 壳（.app）" ON)
if(XIAOTU_BUILD_SHELL AND NOT APPLE)
  message(WARNING "XIAOTU_BUILD_SHELL 仅支持 macOS，当前平台自动关闭")
  set(XIAOTU_BUILD_SHELL OFF)
endif()
message(STATUS "单机版壳：${XIAOTU_BUILD_SHELL}")

add_subdirectory(core)
add_subdirectory(server)
if(XIAOTU_BUILD_SHELL)
  add_subdirectory(shell)
endif()

enable_testing()
add_subdirectory(tests)
