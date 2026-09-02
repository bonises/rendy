# Examples are added per milestone as the features they demo come online.

function(rendy_example name)
  add_executable(${name} ${name}/main.cpp)
  target_link_libraries(${name} PRIVATE rendy::rendy rendy::warnings)
  set_target_properties(${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${name})
endfunction()

rendy_example(01_hello)
rendy_example(02_ui_gallery)
rendy_example(03_text_editor_lite)
rendy_example(04_scene3d)
rendy_example(05_model_viewer)
rendy_example(06_breakout)
rendy_example(07_audio)

# Runtime assets copied next to the binaries.
add_custom_command(TARGET 02_ui_gallery POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/02_ui_gallery/gallery.css
          $<TARGET_FILE_DIR:02_ui_gallery>/gallery.css)

