set(SHELL_SOURCES
  "${CMAKE_SOURCE_DIR}/server/src/cli_main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/mac/webview.mm")

add_executable(xiaotu-inventory MACOSX_BUNDLE ${SHELL_SOURCES})
target_include_directories(xiaotu-inventory PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/mac")
target_link_libraries(xiaotu-inventory PRIVATE xiaotu_server)
target_compile_definitions(xiaotu-inventory PRIVATE XIAOTU_VERSION="${PROJECT_VERSION}"
                                                     XIAOTU_WEBVIEW_SHELL=1)

target_link_libraries(xiaotu-inventory PRIVATE "-framework Cocoa" "-framework WebKit")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/mac/webview.mm"
  PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
# 命令行可执行文件没有 bundle，用 __info_plist 段携带 Info.plist（ATS 允许本地网络）。
set_target_properties(xiaotu-inventory PROPERTIES
  LINK_FLAGS "-Wl,-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/mac/Info.plist")

# 图标：.icns 放进 .app/Contents/Resources，由 Info.plist 的 CFBundleIconFile 指向
set(XIAOTU_ICON "${CMAKE_CURRENT_SOURCE_DIR}/mac/icon/xiaotu.icns")
set_source_files_properties("${XIAOTU_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
target_sources(xiaotu-inventory PRIVATE "${XIAOTU_ICON}")
set_target_properties(xiaotu-inventory PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/mac/Info.plist"
  MACOSX_BUNDLE_ICON_FILE "xiaotu.icns"
  MACOSX_BUNDLE_BUNDLE_NAME "小兔进销存"
  MACOSX_BUNDLE_GUI_IDENTIFIER "com.simem.xiaotu"
  MACOSX_BUNDLE_BUNDLE_VERSION "1"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
