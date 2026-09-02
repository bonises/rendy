# Install support: `cmake --install` ships a SELF-CONTAINED static library
# (rendy + every statically built dependency merged into one librendy.a via
# an ar MRI script), the public headers (rendy/ plus the two public
# dependencies fmt/ and glm/), and a find_package config. Consumers need:
#
#   find_package(rendy CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE rendy::rendy)
#
# Linux-first: archive merging uses `ar -M` through `sh`.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---- the bundle: every static-library target rendy links ------------------
set(_rendy_bundle_targets rendy)
set(_rendy_bundle_candidates
  SDL3-static SDL_uclibc volk fmt freetype harfbuzz yogacore fastgltf
  draco draco_static basisu_transcoder glm)
if(RENDY_SHADER_HOT_RELOAD)
  list(APPEND _rendy_bundle_candidates glslang SPIRV glslang-default-resource-limits
    MachineIndependent GenericCodeGen OSDependent glslang-resource-limits SPVRemapper)
endif()
foreach(_target ${_rendy_bundle_candidates})
  if(TARGET ${_target})
    get_target_property(_type ${_target} TYPE)
    if(_type STREQUAL "STATIC_LIBRARY")
      list(APPEND _rendy_bundle_targets ${_target})
    endif()
  endif()
endforeach()

set(_rendy_mri "create ${CMAKE_BINARY_DIR}/librendy_bundled.a\n")
foreach(_target ${_rendy_bundle_targets})
  string(APPEND _rendy_mri "addlib $<TARGET_FILE:${_target}>\n")
endforeach()
string(APPEND _rendy_mri "save\nend\n")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/rendy_bundle.mri" CONTENT "${_rendy_mri}")

add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/librendy_bundled.a"
  COMMAND sh -c "'${CMAKE_AR}' -M < '${CMAKE_BINARY_DIR}/rendy_bundle.mri'"
  DEPENDS ${_rendy_bundle_targets} "${CMAKE_BINARY_DIR}/rendy_bundle.mri"
  COMMENT "Bundling librendy.a with its static dependencies"
  VERBATIM)
add_custom_target(rendy_bundled ALL DEPENDS "${CMAKE_BINARY_DIR}/librendy_bundled.a")

# ---- install rules --------------------------------------------------------
install(FILES "${CMAKE_BINARY_DIR}/librendy_bundled.a"
        DESTINATION ${CMAKE_INSTALL_LIBDIR} RENAME librendy.a)
install(DIRECTORY include/rendy DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# fmt and glm are rendy's two public dependencies (log.hpp / math.hpp) —
# ship their headers so the installed package stands alone.
install(DIRECTORY "${fmt_SOURCE_DIR}/include/fmt" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY "${glm_SOURCE_DIR}/glm" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h" PATTERN "*.inl")

configure_package_config_file(
  cmake/rendyConfig.cmake.in "${CMAKE_BINARY_DIR}/rendyConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rendy
  PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR)
write_basic_package_version_file(
  "${CMAKE_BINARY_DIR}/rendyConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_BINARY_DIR}/rendyConfig.cmake"
              "${CMAKE_BINARY_DIR}/rendyConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rendy)
