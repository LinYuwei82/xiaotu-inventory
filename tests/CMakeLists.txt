find_package(Threads REQUIRED)

add_executable(xiaotu_core_smoke "${CMAKE_CURRENT_SOURCE_DIR}/core_smoke.cpp")
target_link_libraries(xiaotu_core_smoke PRIVATE xiaotu_core Threads::Threads)

add_test(NAME core_smoke COMMAND xiaotu_core_smoke)

add_executable(xiaotu_config_smoke "${CMAKE_CURRENT_SOURCE_DIR}/config_smoke.cpp")
target_link_libraries(xiaotu_config_smoke PRIVATE xiaotu_server Threads::Threads)

add_test(NAME config_smoke COMMAND xiaotu_config_smoke)

add_executable(xiaotu_session_smoke "${CMAKE_CURRENT_SOURCE_DIR}/session_smoke.cpp")
target_link_libraries(xiaotu_session_smoke PRIVATE xiaotu_server Threads::Threads)

add_test(NAME session_smoke COMMAND xiaotu_session_smoke)

add_executable(xiaotu_platform_paths_smoke "${CMAKE_CURRENT_SOURCE_DIR}/platform_paths_smoke.cpp")
target_link_libraries(xiaotu_platform_paths_smoke PRIVATE xiaotu_server Threads::Threads)

add_test(NAME platform_paths_smoke COMMAND xiaotu_platform_paths_smoke)

add_executable(xiaotu_xlsx_smoke "${CMAKE_CURRENT_SOURCE_DIR}/xlsx_smoke.cpp")
target_link_libraries(xiaotu_xlsx_smoke PRIVATE xiaotu_core Threads::Threads)

add_test(NAME xlsx_smoke
         COMMAND xiaotu_xlsx_smoke "${CMAKE_CURRENT_SOURCE_DIR}/data")

add_executable(xiaotu_masterdata_smoke "${CMAKE_CURRENT_SOURCE_DIR}/masterdata_smoke.cpp")
target_link_libraries(xiaotu_masterdata_smoke PRIVATE xiaotu_server Threads::Threads)

add_test(NAME masterdata_smoke COMMAND xiaotu_masterdata_smoke)

add_executable(xiaotu_opening_smoke "${CMAKE_CURRENT_SOURCE_DIR}/opening_smoke.cpp")
target_link_libraries(xiaotu_opening_smoke PRIVATE xiaotu_server Threads::Threads)

add_test(NAME opening_smoke COMMAND xiaotu_opening_smoke)

add_executable(xiaotu_report_smoke "${CMAKE_CURRENT_SOURCE_DIR}/report_smoke.cpp")
target_link_libraries(xiaotu_report_smoke PRIVATE xiaotu_server Threads::Threads)

add_test(NAME report_smoke COMMAND xiaotu_report_smoke)

add_executable(xiaotu_api_smoke "${CMAKE_CURRENT_SOURCE_DIR}/api_smoke.cpp")
target_link_libraries(xiaotu_api_smoke PRIVATE xiaotu_server Threads::Threads)

add_test(NAME api_smoke COMMAND xiaotu_api_smoke)

add_executable(xiaotu_backup_smoke "${CMAKE_CURRENT_SOURCE_DIR}/backup_smoke.cpp")
target_link_libraries(xiaotu_backup_smoke PRIVATE xiaotu_core Threads::Threads)

add_test(NAME backup_smoke COMMAND xiaotu_backup_smoke)

add_executable(xiaotu_print_paper_smoke "${CMAKE_CURRENT_SOURCE_DIR}/print_paper_smoke.cpp")
target_include_directories(xiaotu_print_paper_smoke
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../shell/mac")

add_test(NAME print_paper_smoke COMMAND xiaotu_print_paper_smoke)

add_executable(xiaotu_update_smoke "${CMAKE_CURRENT_SOURCE_DIR}/update_smoke.cpp")
target_link_libraries(xiaotu_update_smoke PRIVATE xiaotu_core)

add_test(NAME update_smoke COMMAND xiaotu_update_smoke)

add_executable(xiaotu_update_service_smoke
               "${CMAKE_CURRENT_SOURCE_DIR}/update_service_smoke.cpp")
target_link_libraries(xiaotu_update_service_smoke PRIVATE xiaotu_server Threads::Threads)

add_test(NAME update_service_smoke COMMAND xiaotu_update_service_smoke)

if(UNIX)
  # 社区版不支持网络版部署（deployment.network），依赖 --server 的用例不适用。
  if(NOT XIAOTU_EDITION STREQUAL "community")
    add_test(NAME restore_lock_exit
             COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/restore_lock_exit.sh"
                     $<TARGET_FILE:xiaotu-server>)
    add_test(NAME update_api_test
             COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/update_api_test.sh"
                     $<TARGET_FILE:xiaotu-server>)
  endif()
  add_test(NAME deploy_backup_test
           COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/deploy_backup_test.sh"
                   "${CMAKE_CURRENT_SOURCE_DIR}/../deploy/backup.sh")
  add_test(NAME update_manifest_test
           COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/update_manifest_test.sh")
endif()
