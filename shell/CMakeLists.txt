set(SHELL_SOURCES
  "${CMAKE_SOURCE_DIR}/server/src/cli_main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/mac/updater.mm"
  "${CMAKE_CURRENT_SOURCE_DIR}/mac/webview.mm")

# Info.plist 里的版本与构建号由根 VERSION 文件和 XIAOTU_BUILD 注入，
# 不再在 plist 里手写，避免与二进制 / 诊断接口不一致。
set(XIAOTU_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mac/Info.plist" "${XIAOTU_INFO_PLIST}" @ONLY)

add_executable(xiaotu-inventory MACOSX_BUNDLE ${SHELL_SOURCES})
target_include_directories(xiaotu-inventory PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/mac")
target_link_libraries(xiaotu-inventory PRIVATE xiaotu_server)
target_compile_definitions(xiaotu-inventory PRIVATE XIAOTU_VERSION="${PROJECT_VERSION}"
                                                     XIAOTU_WEBVIEW_SHELL=1)

target_link_libraries(xiaotu-inventory PRIVATE "-framework Cocoa" "-framework WebKit")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/mac/webview.mm"
  PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/mac/updater.mm"
  PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

# Sparkle 2 可选：仓库不内嵌框架，发布机通过 -DXIAOTU_SPARKLE_DIR=<含 Sparkle.framework 的目录>
# 接入；未提供时保留可编译的接入点，更新只记录待安装版本。
set(XIAOTU_SPARKLE_DIR "" CACHE PATH "Sparkle.framework 所在目录（可选）")
if(XIAOTU_SPARKLE_DIR AND EXISTS "${XIAOTU_SPARKLE_DIR}/Sparkle.framework")
  target_link_libraries(xiaotu-inventory PRIVATE "-framework Sparkle")
  target_link_options(xiaotu-inventory PRIVATE "-F${XIAOTU_SPARKLE_DIR}")
  target_compile_options(xiaotu-inventory PRIVATE "-F${XIAOTU_SPARKLE_DIR}")
  message(STATUS "Sparkle: ${XIAOTU_SPARKLE_DIR}")
else()
  message(STATUS "Sparkle: 未提供，保留接入点（不自动安装）")
endif()
# 命令行可执行文件没有 bundle，用 __info_plist 段携带生成的 Info.plist（ATS 允许本地网络）。
set_target_properties(xiaotu-inventory PROPERTIES
  LINK_FLAGS "-Wl,-sectcreate,__TEXT,__info_plist,${XIAOTU_INFO_PLIST}")

# 图标：.icns 放进 .app/Contents/Resources，由 Info.plist 的 CFBundleIconFile 指向
set(XIAOTU_ICON "${CMAKE_CURRENT_SOURCE_DIR}/mac/icon/xiaotu.icns")
set_source_files_properties("${XIAOTU_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
target_sources(xiaotu-inventory PRIVATE "${XIAOTU_ICON}")
set_target_properties(xiaotu-inventory PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST "${XIAOTU_INFO_PLIST}"
  MACOSX_BUNDLE_ICON_FILE "xiaotu.icns"
  MACOSX_BUNDLE_BUNDLE_NAME "小兔进销存"
  MACOSX_BUNDLE_GUI_IDENTIFIER "com.simem.xiaotu"
  MACOSX_BUNDLE_BUNDLE_VERSION "${XIAOTU_BUILD}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
