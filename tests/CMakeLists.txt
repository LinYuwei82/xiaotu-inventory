add_executable(xiaotu_core_smoke "${CMAKE_CURRENT_SOURCE_DIR}/core_smoke.cpp")
target_link_libraries(xiaotu_core_smoke PRIVATE xiaotu_core)

add_test(NAME core_smoke COMMAND xiaotu_core_smoke)
