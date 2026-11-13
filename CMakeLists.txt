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
  # 指定初始化器顺序与声明不一致是 C7560。MSVC 14.4x（本机 19.44.35230 实测）默认即
  # 按 error 处理：不带任何 /we 也编译失败（退出码 2），因此不需要 /we7560；而该开关
  # 会被 cl 判为无效参数，每个编译单元刷一条 `D9014: 值"7560"对于"/we"无效`，故移除。
  # Clang/GCC 仍走下面的 -Werror=reorder-init-list 探测。
  # Windows 分发包用静态运行库：安装后不依赖 VC++ Redistributable。
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  # Clang/GCC 默认警告、且不同版本 flag 名不一致；探测到支持才加，release 与 dev 共用。
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-Werror=reorder-init-list" XIAOTU_HAS_REORDER_INIT_LIST)
  if(XIAOTU_HAS_REORDER_INIT_LIST)
    add_compile_options(-Werror=reorder-init-list)
  endif()
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
