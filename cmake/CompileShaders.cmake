# GLSL → SPIR-V → embedded C++ headers, using the FetchContent-built glslang
# standalone tool (no Vulkan SDK required).
#
#   rendy_compile_shaders(rendy shaders/quad2d.vert shaders/quad2d.frag ...)
#
# Each shaders/foo.vert becomes ${build}/generated/shaders/foo_vert_spv.h
# defining `inline constexpr uint32_t foo_vert_spv[]`.

function(rendy_compile_shaders target)
  set(generated_dir ${CMAKE_BINARY_DIR}/generated)
  set(headers)
  foreach(shader IN LISTS ARGN)
    get_filename_component(name ${shader} NAME)
    string(REPLACE "." "_" cname ${name})
    set(cname ${cname}_spv)
    set(spv ${generated_dir}/shaders/${name}.spv)
    set(header ${generated_dir}/shaders/${cname}.h)
    set(source ${CMAKE_CURRENT_SOURCE_DIR}/${shader})

    add_custom_command(
      OUTPUT ${spv}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${generated_dir}/shaders
      COMMAND $<TARGET_FILE:glslang-standalone>
              --target-env vulkan1.3
              -I${CMAKE_CURRENT_SOURCE_DIR}/shaders
              -o ${spv} ${source}
      DEPENDS ${source} glslang-standalone
      COMMENT "glslang ${shader}"
      VERBATIM)
    add_custom_command(
      OUTPUT ${header}
      COMMAND ${CMAKE_COMMAND} -DIN=${spv} -DOUT=${header} -DNAME=${cname}
              -P ${CMAKE_SOURCE_DIR}/cmake/Bin2H.cmake
      DEPENDS ${spv} ${CMAKE_SOURCE_DIR}/cmake/Bin2H.cmake
      COMMENT "bin2h ${name}"
      VERBATIM)
    list(APPEND headers ${header})
  endforeach()

  add_custom_target(${target}_shaders DEPENDS ${headers})
  add_dependencies(${target} ${target}_shaders)
  target_include_directories(${target} PRIVATE ${generated_dir})
endfunction()
