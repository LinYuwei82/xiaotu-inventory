set(THIRD_PARTY "${CMAKE_SOURCE_DIR}/third_party")
include("${CMAKE_SOURCE_DIR}/cmake/generated_sources.cmake")

add_library(xiaotu_server STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src/apply_update.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/audit_routes.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/backup_routes.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/config.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/document_routes.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/finance_routes.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/http_guard.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/http_json.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/http_server.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/instance_lock.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/login_limiter.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/masterdata_routes.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/platform_paths.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/partner_routes.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/print_routes.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/process_capture.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/report_routes.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/restore_wizard.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/routes.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/session_store.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/setting_routes.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/stock_routes.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/system_shutdown.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/update_install.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/update_service.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/user_routes.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/xlsx.cpp"
  ${XIAOTU_GENERATED_SERVER_SOURCES})

target_include_directories(xiaotu_server PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(xiaotu_server PUBLIC xiaotu_core)

# 服务端命令行入口：不依赖 WebView 壳，Windows / 无壳构建用它在浏览器里开发。
add_executable(xiaotu-server "${CMAKE_CURRENT_SOURCE_DIR}/src/cli_main.cpp")
target_link_libraries(xiaotu-server PRIVATE xiaotu_server)
target_compile_definitions(xiaotu-server PRIVATE XIAOTU_VERSION="${PROJECT_VERSION}")

# Windows 下给控制台 exe 写版本资源，让「属性 → 详细信息」能看到 FileVersion。
# 版本号取项目级 PROJECT_VERSION（来自仓库根 VERSION），与壳和 XIAOTU_VERSION 宏同源。
if(WIN32)
  # apply_update 需要 version.lib（GetFileVersionInfo）与 shell32.lib（CommandLineToArgvW）。
  # 挂在静态库上 PUBLIC，让链接 xiaotu_server 的测试/壳也自动带上。
  target_link_libraries(xiaotu_server PUBLIC version shell32)
  # 服务端 exe 与壳、安装包使用同一份候选 A 图标。
  set(XIAOTU_WIN_ICON "${CMAKE_SOURCE_DIR}/packaging/win/xiaotu.ico")
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
  target_sources(xiaotu-server PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
  # version.rc 只引用 ico 路径，内容不随图标变化；显式声明依赖，
  # 换图标后 reconfigure + build 才会重新编译资源，不必手动 clean。
  set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    PROPERTIES OBJECT_DEPENDS "${XIAOTU_WIN_ICON}")
endif()

if(XIAOTU_DEV_WEB)
  target_compile_definitions(xiaotu_server PUBLIC XIAOTU_DEV_WEB=1)
endif()

# 优先使用 vcpkg 的 cpp-httplib，缺失时回落到随仓库内嵌的头文件。
find_package(httplib CONFIG QUIET)
if(httplib_FOUND)
  target_link_libraries(xiaotu_server PUBLIC httplib::httplib)
  message(STATUS "cpp-httplib: vcpkg (${httplib_DIR})")
else()
  target_include_directories(xiaotu_server PUBLIC "${THIRD_PARTY}/httplib")
  message(STATUS "cpp-httplib: third_party 内嵌头文件")
endif()
