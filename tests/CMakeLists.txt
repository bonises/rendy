add_executable(rendy_tests
  core/test_result.cpp
  core/test_handle.cpp
  core/test_color.cpp
  core/test_rect.cpp
  math/test_math.cpp
  text/test_utf8.cpp
  css/test_tokenizer.cpp
  css/test_parser.cpp
  css/test_cascade.cpp
  layout/test_layout.cpp
  scene/test_animation.cpp
  audio/test_mixer.cpp
)
target_include_directories(rendy_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rendy_tests PRIVATE yogacore)

target_link_libraries(rendy_tests PRIVATE rendy::rendy Catch2::Catch2WithMain rendy::warnings)

include(Catch)
catch_discover_tests(rendy_tests)
