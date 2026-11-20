# 跨平台 UTF-8 边界模块。被父工程 add_subdirectory 引入，不单独 project()。
#
# Windows 侧直接调 Win32（MultiByteToWideChar / SetConsoleCP / FormatMessageW），
# POSIX 侧是同语义的 UTF-8 ↔ UTF-32 实现，因此调用方不需要 #ifdef。

add_library(win_encoding STATIC
  "${CMAKE_CURRENT_LIST_DIR}/src/encoding.cpp")

target_include_directories(win_encoding PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/include")

target_compile_features(win_encoding PUBLIC cxx_std_20)

if(MSVC)
  # 源码含中文字面量（"系统错误"），必须按 UTF-8 解析。
  target_compile_options(win_encoding PRIVATE /utf-8 /W4)
  target_compile_definitions(win_encoding PRIVATE UNICODE _UNICODE)
else()
  target_compile_options(win_encoding PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wall>")
endif()
