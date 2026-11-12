set(SHELL_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/common/platform_paths.cpp")

if(APPLE)
  list(APPEND SHELL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/mac/webview.mm")
endif()

add_executable(xiaotu-inventory ${SHELL_SOURCES})
target_include_directories(xiaotu-inventory PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common")
target_link_libraries(xiaotu-inventory PRIVATE xiaotu_server)
target_compile_definitions(xiaotu-inventory PRIVATE XIAOTU_VERSION="${PROJECT_VERSION}")

if(XIAOTU_DEV_WEB)
  target_compile_definitions(xiaotu-inventory PRIVATE XIAOTU_DEV_WEB=1)
endif()

if(APPLE)
  target_include_directories(xiaotu-inventory PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/mac")
  target_link_libraries(xiaotu-inventory PRIVATE "-framework Cocoa" "-framework WebKit")
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/mac/webview.mm"
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  # 命令行可执行文件没有 bundle，用 __info_plist 段携带 Info.plist（ATS 允许本地网络）。
  set_target_properties(xiaotu-inventory PROPERTIES
    LINK_FLAGS "-Wl,-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/mac/Info.plist")
endif()
