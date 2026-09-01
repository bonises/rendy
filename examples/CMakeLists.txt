# Examples are added per milestone as the features they demo come online.

function(rendy_example name)
  add_executable(${name} ${name}/main.cpp)
  target_link_libraries(${name} PRIVATE rendy::rendy rendy::warnings)
  set_target_properties(${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${name})
endfunction()

rendy_example(01_hello)
rendy_example(03_text_editor_lite)
