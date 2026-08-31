cmake_minimum_required(VERSION 3.28)
project(rendy VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(RENDY_BUILD_EXAMPLES "Build the example apps" ON)
option(RENDY_BUILD_TESTS "Build the unit tests" ON)
option(RENDY_WERROR "Treat warnings as errors (CI)" OFF)
option(RENDY_VULKAN_VALIDATION "Enable Vulkan validation layers by default" OFF)
option(RENDY_SHADER_HOT_RELOAD "Link glslang into rendy for runtime shader reload" OFF)
set(RENDY_SANITIZE "" CACHE STRING "Semicolon list of sanitizers, e.g. address;undefined")

include(cmake/Warnings.cmake)
include(cmake/Dependencies.cmake)

# ------------------------------------------------------------------ library
add_library(rendy STATIC
  src/core/log.cpp
)
add_library(rendy::rendy ALIAS rendy)

target_include_directories(rendy PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(rendy PRIVATE src)

target_link_libraries(rendy
  PUBLIC fmt::fmt glm::glm
  PRIVATE rendy::warnings)

target_compile_features(rendy PUBLIC cxx_std_20)

# ----------------------------------------------------------------- examples
if(RENDY_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# -------------------------------------------------------------------- tests
if(RENDY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
