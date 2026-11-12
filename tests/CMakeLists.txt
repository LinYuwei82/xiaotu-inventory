find_package(Threads REQUIRED)

add_executable(xiaotu_core_smoke "${CMAKE_CURRENT_SOURCE_DIR}/core_smoke.cpp")
target_link_libraries(xiaotu_core_smoke PRIVATE xiaotu_core Threads::Threads)

add_test(NAME core_smoke COMMAND xiaotu_core_smoke)
