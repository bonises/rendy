# Standalone consumer of an INSTALLED rendy package — configured as its own
# project (never via add_subdirectory) against a prefix:
#
#   cmake --install build/release --prefix /tmp/rendy-prefix
#   cmake -S tests/install -B /tmp/consumer -DCMAKE_PREFIX_PATH=/tmp/rendy-prefix
#   cmake --build /tmp/consumer && /tmp/consumer/rendy_consumer
#
# CI builds it (link check, no display); locally it runs with RENDY_AUTOQUIT.
cmake_minimum_required(VERSION 3.28)
project(rendy_consumer CXX)

find_package(rendy CONFIG REQUIRED)

add_executable(rendy_consumer main.cpp)
target_link_libraries(rendy_consumer PRIVATE rendy::rendy)
