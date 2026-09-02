add_executable(rendy_tests
  lsan_suppressions.cpp
  core/test_result.cpp
  core/test_handle.cpp
  core/test_color.cpp
  core/test_rect.cpp
  math/test_math.cpp
  text/test_utf8.cpp
  text/test_shaper.cpp
  text/test_caret.cpp
  css/test_tokenizer.cpp
  css/test_parser.cpp
  css/test_cascade.cpp
  css/test_transitions.cpp
  css/test_animations.cpp
  canvas/test_snapshot.cpp
  layout/test_layout.cpp
  layout/test_text_edit.cpp
  scene/test_animation.cpp
  scene/test_block_allocator.cpp
  audio/test_mixer.cpp
)
target_include_directories(rendy_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rendy_tests PRIVATE yogacore)

target_link_libraries(rendy_tests PRIVATE rendy::rendy Catch2::Catch2WithMain rendy::warnings)

include(Catch)
catch_discover_tests(rendy_tests)

# GPU smoke tests: need a Vulkan device + windowing (run locally, not CI).
# ctest -L gpu, or the rendy_gpu_tests binary directly.
option(RENDY_GPU_TESTS "Build GPU smoke tests (needs a Vulkan device)" OFF)
if(RENDY_GPU_TESTS)
  add_executable(rendy_gpu_tests gpu/test_gpu.cpp)
  target_include_directories(rendy_gpu_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(rendy_gpu_tests PRIVATE rendy::rendy Catch2::Catch2WithMain
                                                rendy::warnings)
  catch_discover_tests(rendy_gpu_tests PROPERTIES LABELS gpu RUN_SERIAL ON)
endif()
