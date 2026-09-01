add_executable(rendy_tests
  core/test_result.cpp
  core/test_handle.cpp
  core/test_color.cpp
  core/test_rect.cpp
  math/test_math.cpp
  text/test_utf8.cpp
)
target_include_directories(rendy_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(rendy_tests PRIVATE rendy::rendy Catch2::Catch2WithMain rendy::warnings)

include(Catch)
catch_discover_tests(rendy_tests)
