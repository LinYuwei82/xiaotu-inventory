# 小兔进销存 Windows 桌面壳
#
# 两种用法：
#   1) 独立工程（默认）：
#        .\shell\win\fetch-webview2.ps1
#        $env:XIAOTU_BUILD = "<gen_build.py 生成>"
#        cmake -S shell\win -B shell\win\build -G Ninja -DCMAKE_BUILD_TYPE=Release -DXIAOTU_RELEASE_BUILD=ON
#        cmake --build shell\win\build
#      发布构建必须带 -DXIAOTU_RELEASE_BUILD=ON（或环境变量 XIAOTU_BUILD），
#      缺构建号或使用开发占位值时 configure 阶段直接失败。
#   2) 接入顶层构建（仅 Windows，默认关闭）：
#        cmake --preset windows-release -DXIAOTU_BUILD_WIN_SHELL=ON
#
# 产物：xiaotu-shell.exe（需与 xiaotu-server.exe 同目录运行）
cmake_minimum_required(VERSION 3.25)

# 产品版本沿用仓库根 VERSION.txt，避免 Windows 壳另写一份。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION.txt" XIAOTU_VERSION_STRING)
string(STRIP "${XIAOTU_VERSION_STRING}" XIAOTU_VERSION_STRING)

# 作为顶层工程的子目录时不重复 project()，沿用父工程的编译器、标准与运行库设置。
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(xiaotu_shell_win VERSION ${XIAOTU_VERSION_STRING} LANGUAGES CXX)
endif()

if (NOT WIN32)
  message(FATAL_ERROR "shell/win 只能在 Windows 上构建")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 与顶层工程一致：静态运行库（Release /MT、Debug /MTd），不依赖 VC++ Redistributable。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(XIAOTU_WEBVIEW2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.cache/webview2" CACHE PATH
    "WebView2 SDK 目录（由 fetch-webview2.ps1 准备）")

set(_wv2_header "${XIAOTU_WEBVIEW2_DIR}/include/WebView2.h")
set(_wv2_lib "${XIAOTU_WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib")

if (NOT EXISTS "${_wv2_header}" OR NOT EXISTS "${_wv2_lib}")
  message(FATAL_ERROR
    "未找到 WebView2 SDK。请先运行：\n"
    "  powershell -ExecutionPolicy Bypass -File \"${CMAKE_CURRENT_SOURCE_DIR}/fetch-webview2.ps1\"")
endif()

# ---- core 更新模块（独立工程直接 add 源文件，顶层构建时用 xiaotu_core target） ----
set(_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(_core_include "${_repo_root}/core/include")
set(_third_party "${_repo_root}/third_party")

if (TARGET xiaotu_core)
  set(_core_link xiaotu_core)
else()
  add_library(xiaotu_core_updater STATIC
    "${_repo_root}/core/src/update.cpp"
    "${_repo_root}/core/src/update_signature.cpp"
    "${_repo_root}/core/src/log.cpp")
  target_include_directories(xiaotu_core_updater PUBLIC
    "${_core_include}"
    "${_third_party}"
    "${_repo_root}/cmake")
  # 独立构建没有父工程的 -DXIAOTU_BUILD，改从同名 CMake 变量 / 环境变量读，
  # 与仓库根 CMakeLists 同一套语义：XIAOTU_RELEASE_BUILD=ON 时缺构建号、非数字或
  # 仍为开发占位值都直接 FATAL；本地开发回落到 999999999 并告警。
  # 发布包必须与服务端、清单使用同一个 build 号：core 的更新判定是
  # `manifest.build <= 当前 build → 无更新`，若壳的 build 落后于清单，
  # 客户端会每次都提示有新版本、装完仍然提示。
  option(XIAOTU_RELEASE_BUILD "发布/出包构建：XIAOTU_BUILD 必须显式提供（由 gen_build.py 生成）" OFF)
  set(XIAOTU_DEFAULT_BUILD "999999999")
  # 来源优先级：-DXIAOTU_BUILD → 环境变量 XIAOTU_BUILD（tools\package\package_windows.ps1 用这个）
  set(_build_epoch "${XIAOTU_BUILD}")
  if (_build_epoch STREQUAL "" AND NOT "$ENV{XIAOTU_BUILD}" STREQUAL "")
    set(_build_epoch "$ENV{XIAOTU_BUILD}")
  endif()
  if (_build_epoch STREQUAL "")
    if (XIAOTU_RELEASE_BUILD)
      message(FATAL_ERROR "发布/出包构建必须显式指定 -DXIAOTU_BUILD=<build> 或环境变量 XIAOTU_BUILD，推荐：\n  BUILD=$(python3 tools/release/gen_build.py --product xiaotu --repo-root .)\n  cmake -S shell/win -B shell/win/build -DXIAOTU_RELEASE_BUILD=ON -DXIAOTU_BUILD=$BUILD ...")
    endif()
    set(_build_epoch "${XIAOTU_DEFAULT_BUILD}")
    message(WARNING
      "未设置 XIAOTU_BUILD，壳的构建号回退为开发占位值 ${_build_epoch}（仅适合本地开发）。"
      "发布包请用 -DXIAOTU_RELEASE_BUILD=ON 并显式传入构建号。")
  endif()
  if (NOT _build_epoch MATCHES "^[0-9]+$")
    message(FATAL_ERROR "XIAOTU_BUILD 必须是纯数字：${_build_epoch}")
  endif()
  if (XIAOTU_RELEASE_BUILD AND _build_epoch STREQUAL XIAOTU_DEFAULT_BUILD)
    message(FATAL_ERROR "发布构建（XIAOTU_RELEASE_BUILD=ON）不能使用开发默认构建号 ${XIAOTU_DEFAULT_BUILD}，请用 gen_build.py 生成")
  endif()
  # 只对本次配置生效：避免缓存把发布态带到同一目录后续的开发构建。
  unset(XIAOTU_RELEASE_BUILD CACHE)
  # 让下面的壳目标 compile_definitions 也能拿到
  set(XIAOTU_BUILD "${_build_epoch}")
  target_compile_definitions(xiaotu_core_updater PUBLIC
    XIAOTU_VERSION="${XIAOTU_VERSION_STRING}"
    XIAOTU_BUILD=${_build_epoch}
    XIAOTU_BUILD_STR="${_build_epoch}"
    XIAOTU_API_VERSION=1
    XIAOTU_EDITION_CODE="full")
  target_compile_options(xiaotu_core_updater PUBLIC /utf-8)
  set(_core_link xiaotu_core_updater)
endif()

# OpenSSL 3.0（Ed25519 验签），独立工程和顶层都要
find_package(OpenSSL 3.0 REQUIRED)
if (NOT TARGET xiaotu_core)
  target_link_libraries(xiaotu_core_updater PRIVATE OpenSSL::SSL OpenSSL::Crypto bcrypt)
  target_compile_definitions(xiaotu_core_updater PUBLIC XIAOTU_HAS_OPENSSL=1)
endif()
message(STATUS "OpenSSL (Ed25519): ${OPENSSL_VERSION}")

# trust anchor 公钥（Ed25519，base64，32 字节），与 macOS Info.plist SUPublicEDKey 同源。
# 默认值来自 build 机 .config/xiaotu-updates/update_signing_2026.pub.pem 取 DER 最后 32 字节 base64。
# 构建时可用 -DXIAOTU_UPDATE_PUBKEY=<base64> 覆盖，换密钥时只需改这一处。
set(XIAOTU_UPDATE_PUBKEY "HgCDvW0MQ32HFguQ/jW5fVtQh74lBsXMsyfo0sRLH1Y=" CACHE STRING "更新清单 Ed25519 信任锚公钥（base64）")
if (XIAOTU_UPDATE_PUBKEY)
  message(STATUS "更新公钥：已配置")
else()
  message(STATUS "更新公钥：未配置（将跳过 C++ 侧验签）")
endif()

add_executable(xiaotu-shell WIN32 main.cpp updater_win.cpp app.manifest)

# 版本与图标资源：产品版本来自根 VERSION，图标与 macOS 同为候选 A。
set(XIAOTU_WIN_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../../packaging/win/xiaotu.ico")
if (NOT EXISTS "${XIAOTU_WIN_ICON}")
  message(FATAL_ERROR "未找到 Windows 图标：${XIAOTU_WIN_ICON}")
endif()
# 传给 rc.exe 的路径保持正斜杠：反斜杠会被 RC 当作转义，路径在 \x 处被截断，
# 报 RC2135 file not found: D:\projects。
file(TO_CMAKE_PATH "${XIAOTU_WIN_ICON}" XIAOTU_WIN_ICON_RC)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
  @ONLY)
target_sources(xiaotu-shell PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
# version.rc 只引用 ico 路径，内容不随图标变化；显式声明依赖，
# 换图标后 reconfigure + build 才会重新编译资源，不必手动 clean。
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/version.rc"
  PROPERTIES OBJECT_DEPENDS "${XIAOTU_WIN_ICON}")

target_include_directories(xiaotu-shell PRIVATE
  "${XIAOTU_WEBVIEW2_DIR}/include"
  "${_core_include}"
  "${_third_party}"
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_directories(xiaotu-shell PRIVATE "${XIAOTU_WEBVIEW2_DIR}/build/native/x64")

# 源码含中文字面量，MSVC 默认按系统代码页解析会出错。
target_compile_options(xiaotu-shell PRIVATE /utf-8 /W4 /EHsc)
target_compile_definitions(xiaotu-shell PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
                                       XIAOTU_UPDATE_PUBKEY="${XIAOTU_UPDATE_PUBKEY}"
                                       XIAOTU_BUILD_STR="${XIAOTU_BUILD}")

target_link_libraries(xiaotu-shell PRIVATE
  WebView2LoaderStatic
  ${_core_link}
  OpenSSL::SSL OpenSSL::Crypto
  winhttp bcrypt
  ole32 oleaut32 user32 shell32 gdi32)

# 头文件拷到产物目录旁，方便与 xiaotu-server.exe 一起分发。
add_custom_command(TARGET xiaotu-shell POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
          "$<TARGET_FILE_DIR:xiaotu-shell>/shell-README.md"
  VERBATIM)
