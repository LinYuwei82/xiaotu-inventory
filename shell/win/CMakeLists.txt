# 小兔进销存 Windows 桌面壳（独立工程）
#
# 刻意不接入顶层 CMake 构建体系：根 CMakeLists.txt / CMakePresets.json /
# shell/CMakeLists.txt 一律不改，这里自带 configure 入口。
#
#   .\shell\win\fetch-webview2.ps1
#   cmake -S shell\win -B shell\win\build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build shell\win\build
#
# 产物：shell\win\build\xiaotu-shell.exe（需与 xiaotu-server.exe 同目录运行）
cmake_minimum_required(VERSION 3.25)

# 产品版本沿用仓库根 VERSION 文件，避免 Windows 壳另写一份。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" XIAOTU_VERSION_STRING)
string(STRIP "${XIAOTU_VERSION_STRING}" XIAOTU_VERSION_STRING)
project(xiaotu_shell_win VERSION ${XIAOTU_VERSION_STRING} LANGUAGES CXX)

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

add_executable(xiaotu-shell WIN32 main.cpp)

# 版本资源：产品版本来自根 VERSION，随 exe 的「属性 → 详细信息」展示。
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
  @ONLY)
target_sources(xiaotu-shell PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version.rc")

target_include_directories(xiaotu-shell PRIVATE "${XIAOTU_WEBVIEW2_DIR}/include")
target_link_directories(xiaotu-shell PRIVATE "${XIAOTU_WEBVIEW2_DIR}/build/native/x64")

# 源码含中文字面量，MSVC 默认按系统代码页解析会出错。
target_compile_options(xiaotu-shell PRIVATE /utf-8 /W4 /EHsc)
target_compile_definitions(xiaotu-shell PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

target_link_libraries(xiaotu-shell PRIVATE
  WebView2LoaderStatic
  ole32 oleaut32 user32 shell32 gdi32)

# 头文件拷到产物目录旁，方便与 xiaotu-server.exe 一起分发。
add_custom_command(TARGET xiaotu-shell POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
          "$<TARGET_FILE_DIR:xiaotu-shell>/shell-README.md"
  VERBATIM)
