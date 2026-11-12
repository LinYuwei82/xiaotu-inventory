set(THIRD_PARTY "${CMAKE_SOURCE_DIR}/third_party")
set(WEB_DIR "${CMAKE_SOURCE_DIR}/web")
include("${CMAKE_SOURCE_DIR}/cmake/generated_sources.cmake")

option(XIAOTU_DEV_WEB "开发态：前端由本地 dev server 提供，不内嵌进二进制" OFF)

if(XIAOTU_DEV_WEB)
  set(EMBEDDED_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/embedded_web.cpp")
  file(WRITE "${EMBEDDED_SOURCE}"
"// 开发态占位：不内嵌前端资源，WebView 由 XIAOTU_DEV_URL 指向本地 dev server。
#include \"xiaotu/resources.h\"

namespace xiaotu::resources {

bool find(std::string_view, Resource&) { return false; }

std::size_t count() { return 0; }

}  // namespace xiaotu::resources
")
  message(STATUS "前端：开发态（不内嵌资源）")
else()
  # 构建期把 web/ 下的全部静态文件嵌入二进制；新增生成页面后自动重新配置。
  file(GLOB_RECURSE WEB_ASSET_FILES CONFIGURE_DEPENDS
    RELATIVE "${WEB_DIR}"
    "${WEB_DIR}/*.html" "${WEB_DIR}/*.js" "${WEB_DIR}/*.css" "${WEB_DIR}/*.json")
  string(REPLACE ";" "|" WEB_FILES "${WEB_ASSET_FILES}")

  set(EMBEDDED_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/embedded_web.cpp")
  set(EMBED_DEPENDS "${CMAKE_SOURCE_DIR}/cmake/embed_resources.cmake")
  foreach(file IN LISTS WEB_ASSET_FILES)
    list(APPEND EMBED_DEPENDS "${WEB_DIR}/${file}")
  endforeach()

  add_custom_command(
    OUTPUT "${EMBEDDED_SOURCE}"
    COMMAND "${CMAKE_COMMAND}"
            "-DWEB_DIR=${WEB_DIR}"
            "-DFILES=${WEB_FILES}"
            "-DOUT=${EMBEDDED_SOURCE}"
            -P "${CMAKE_SOURCE_DIR}/cmake/embed_resources.cmake"
    DEPENDS ${EMBED_DEPENDS}
    COMMENT "嵌入前端静态资源"
    VERBATIM)
  message(STATUS "前端：发布态（内嵌 ${WEB_ASSET_FILES}）")
endif()

# SQLite 官方 amalgamation 直接编入二进制，不依赖系统库。
set(SQLITE_SOURCE "${THIRD_PARTY}/sqlite/sqlite3.c")
set_source_files_properties("${SQLITE_SOURCE}" PROPERTIES COMPILE_OPTIONS "-w")

# miniz 只用于读取 xlsx（inflate）：关掉压缩与 ZIP 归档 API，避免白编一份 deflate。
set(MINIZ_DIR "${THIRD_PARTY}/miniz")
set(MINIZ_SOURCE "${MINIZ_DIR}/miniz.c")
set_source_files_properties("${MINIZ_SOURCE}" PROPERTIES COMPILE_OPTIONS "-w")

# Argon2 官方参考实现内嵌编译，不依赖系统库或包管理器。
set(ARGON2_DIR "${THIRD_PARTY}/argon2")
set(ARGON2_SOURCES
  "${ARGON2_DIR}/src/argon2.c"
  "${ARGON2_DIR}/src/core.c"
  "${ARGON2_DIR}/src/encoding.c"
  "${ARGON2_DIR}/src/ref.c"
  "${ARGON2_DIR}/src/thread.c"
  "${ARGON2_DIR}/src/blake2/blake2b.c")
set_source_files_properties(${ARGON2_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

add_library(xiaotu_core STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src/audit.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/context.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/backup.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/cost_recalc.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/log.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/masterdata_io.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/crud.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/db.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/doc_number.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/document.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/edition.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/finance_service.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ids.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/item_category.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/migrations.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/opening_import.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/money.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/password.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/permissions.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/print_template.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/report_service.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/settlement_service.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/stock_service.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/token_hash.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/user_service.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/xlsx_reader.cpp"
  ${XIAOTU_GENERATED_CORE_SOURCES}
  "${SQLITE_SOURCE}"
  "${MINIZ_SOURCE}"
  ${ARGON2_SOURCES}
  "${EMBEDDED_SOURCE}")

target_include_directories(xiaotu_core
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PUBLIC "${THIRD_PARTY}/sqlite"
  PRIVATE "${MINIZ_DIR}"
  PRIVATE "${ARGON2_DIR}/include"
  PRIVATE "${ARGON2_DIR}/src")

find_package(Threads REQUIRED)
target_link_libraries(xiaotu_core PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(xiaotu_core PRIVATE bcrypt)
endif()

# 生成的能力表按这个宏选择生效版本，见 core/include/xiaotu/gen/edition_caps.h。
target_compile_definitions(xiaotu_core PUBLIC
  XIAOTU_EDITION_CODE="${XIAOTU_EDITION}")

target_compile_definitions(xiaotu_core PRIVATE
  SQLITE_THREADSAFE=1
  SQLITE_DQS=0
  SQLITE_DEFAULT_FOREIGN_KEYS=1
  SQLITE_OMIT_LOAD_EXTENSION=1
  MINIZ_NO_DEFLATE_APIS=1
  MINIZ_NO_ARCHIVE_APIS=1
  MINIZ_NO_ZLIB_APIS=1
  MINIZ_NO_STDIO=1
  MINIZ_NO_TIME=1)

# 优先使用 vcpkg 提供的 nlohmann-json，缺失时回落到随仓库内嵌的头文件。
find_package(nlohmann_json CONFIG QUIET)
if(nlohmann_json_FOUND)
  target_link_libraries(xiaotu_core PUBLIC nlohmann_json::nlohmann_json)
  message(STATUS "nlohmann/json: vcpkg (${nlohmann_json_DIR})")
else()
  target_include_directories(xiaotu_core PUBLIC "${THIRD_PARTY}")
  message(STATUS "nlohmann/json: third_party 内嵌头文件")
endif()
