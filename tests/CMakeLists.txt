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
