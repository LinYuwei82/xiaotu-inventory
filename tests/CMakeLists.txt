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
