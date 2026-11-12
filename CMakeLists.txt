cmake_minimum_required(VERSION 3.25)

project(xiaotu_inventory VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE)
  enable_language(OBJCXX)
  if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "macOS 最低版本")
  endif()
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "构建类型" FORCE)
endif()

# 版本：full（全功能版，默认）或 community（社区版）。
# 能力与额度取自 tools/codegen/schema/editions.json，这里只做选择，不写死数值。
set(XIAOTU_EDITION "full" CACHE STRING "版本：full（全功能版）或 community（社区版）")
message(STATUS "版本：${XIAOTU_EDITION}")

add_subdirectory(core)
add_subdirectory(server)
add_subdirectory(shell)

enable_testing()
add_subdirectory(tests)
