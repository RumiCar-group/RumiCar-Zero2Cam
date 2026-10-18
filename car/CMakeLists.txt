cmake_minimum_required(VERSION 3.25)
project(RumiCarZero2Cam VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(RUMICAR_BUILD_CORE "Build rumicar-core (needs libgpiod >= 2, OpenSSL)" ON)
option(RUMICAR_BUILD_TESTS "Build unit tests" ON)

add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion)

find_package(nlohmann_json 3.11 REQUIRED)

# Shared contract header (C ABI between core and logic plug-ins).
add_library(rumicar_api INTERFACE)
target_include_directories(rumicar_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_subdirectory(logic)

if(RUMICAR_BUILD_CORE)
  add_subdirectory(core)
endif()

if(RUMICAR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/packs/base DESTINATION share/rumicar/packs)
