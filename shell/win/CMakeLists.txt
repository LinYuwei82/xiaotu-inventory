# 小兔进销存 Windows 桌面壳
#
# 两种用法：
#   1) 独立工程（默认）：
#        .\shell\win\fetch-webview2.ps1
#        cmake -S shell\win -B shell\win\build -G Ninja -DCMAKE_BUILD_TYPE=Release
#        cmake --build shell\win\build
#   2) 接入顶层构建（仅 Windows，默认关闭）：
#        cmake --preset windows-release -DXIAOTU_BUILD_WIN_SHELL=ON
#
# 产物：xiaotu-shell.exe（需与 xiaotu-server.exe 同目录运行）
cmake_minimum_required(VERSION 3.25)

# 产品版本沿用仓库根 VERSION 文件，避免 Windows 壳另写一份。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" XIAOTU_VERSION_STRING)
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

add_executable(xiaotu-shell WIN32 main.cpp)

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
