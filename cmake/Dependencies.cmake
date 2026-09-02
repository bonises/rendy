# All third-party dependencies, fetched and pinned here.
# No system packages are required beyond the Vulkan loader (libvulkan.so.1)
# and the X11/ALSA dev libraries SDL picks up at configure time.

include(FetchContent)

# Some pinned deps declare cmake_minimum_required < 3.5, which CMake 4 refuses
# to configure without this override.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------- fmt 11.0.2
FetchContent_Declare(fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt
  GIT_TAG 0c9fce2ffefecfdce794e1859584e25877b7b592 # 11.0.2
  SYSTEM EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------- GLM 1.0.1
FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm
  GIT_TAG 0af55ccecd98d4e5a8d1fad7de25ba429d60e863 # 1.0.1
  SYSTEM EXCLUDE_FROM_ALL)

# ------------------------------------------------------------- Catch2 v3.8.0
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2
  GIT_TAG f7cfc885ba68c317d20cbbfa6b8aa8ecbcbde19f # v3.8.0
  SYSTEM EXCLUDE_FROM_ALL)

# --------------------------------------------------------- SDL release-3.2.30
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL
  GIT_TAG release-3.2.30
  SYSTEM EXCLUDE_FROM_ALL)

# ------------------------------------------------- Vulkan-Headers v1.3.296
# The C++20 module target auto-enables on new-enough compilers (clang 16+)
# but requires the Ninja generator — hard configure error under Makefiles.
# We consume the C headers only.
set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(VulkanHeaders
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers
  GIT_TAG 1d9bcc9af77d93ba355b15994b9f82a130e9df3a # v1.3.296
  SYSTEM EXCLUDE_FROM_ALL)

# ------------------------------------------------- volk (matching headers)
set(VOLK_PULL_IN_VULKAN ON CACHE BOOL "" FORCE)
FetchContent_Declare(volk
  GIT_REPOSITORY https://github.com/zeux/volk
  GIT_TAG 59d26900f53c7621a8ba8ab0e3f18d3bd883fa9a # vulkan-sdk-1.3.296.0
  SYSTEM EXCLUDE_FROM_ALL)

# ------------------------------------- VulkanMemoryAllocator v3.2.1 (header)
FetchContent_Declare(VulkanMemoryAllocator
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
  GIT_TAG c788c52156f3ef7bc7ab769cb03c110a53ac8fcb # v3.2.1
  SYSTEM EXCLUDE_FROM_ALL)

# ------------------------------------------------------------ glslang 15.1.0
# ENABLE_OPT=OFF avoids the SPIRV-Tools dependency; drivers optimize anyway.
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
# ASan's ODR/global metadata references typeinfo of glslang classes used in
# shader_compiler.cpp — without RTTI in glslang the asan preset fails to link.
set(ENABLE_RTTI ON CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
  GIT_REPOSITORY https://github.com/KhronosGroup/glslang
  GIT_TAG 1062752a891c95b2bfeed9e356562d88f9df84ac # 15.1.0
  SYSTEM EXCLUDE_FROM_ALL)

# ----------------------------------------------------- FreeType VER-2-13-3
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_Declare(freetype
  GIT_REPOSITORY https://github.com/freetype/freetype
  GIT_TAG 42608f77f20749dd6ddc9e0536788eaad70ea4b5 # VER-2-13-3
  SYSTEM EXCLUDE_FROM_ALL)

# --------------------------------------------------------- HarfBuzz 10.2.0
# Core shaping only (hb-ot font funcs — no FreeType/GLib/ICU coupling);
# FreeType stays the rasterizer, HarfBuzz provides glyph mapping + advances.
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
FetchContent_Declare(harfbuzz
  GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz
  GIT_TAG 818890f8f6c364ed111689a40ad510c415e559a1 # 10.2.0
  SYSTEM EXCLUDE_FROM_ALL)

# ------------------------------------------------------------- Yoga v3.2.1
FetchContent_Declare(yoga
  GIT_REPOSITORY https://github.com/facebook/yoga
  GIT_TAG 042f5013152eb81c1552dec945b88f7b95ca350f # v3.2.1
  SYSTEM EXCLUDE_FROM_ALL)

# ---------------------------------------------------------- fastgltf v0.9.0
FetchContent_Declare(fastgltf
  GIT_REPOSITORY https://github.com/spnda/fastgltf
  GIT_TAG 0d1b67a28c4950ea2deb796702006dcbe31e02b3 # v0.9.0
  SYSTEM EXCLUDE_FROM_ALL)

# -------------------------------------------------------------- draco 1.5.7
# Decodes KHR_draco_mesh_compression glTF primitives.
set(DRACO_JS_GLUE OFF CACHE BOOL "" FORCE)
set(DRACO_TESTS OFF CACHE BOOL "" FORCE)
set(DRACO_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(draco
  GIT_REPOSITORY https://github.com/google/draco
  GIT_TAG 8786740086a9f4d83f44aa83badfbea4dce7a1b5 # 1.5.7
  SYSTEM EXCLUDE_FROM_ALL)

# --------------------------------------- basis_universal 1.16.4 (transcoder)
# Only the transcoder is compiled (KTX2/KHR_texture_basisu → BC7); the
# repo's own CMakeLists builds the full encoder tool, so it is skipped via a
# nonexistent SOURCE_SUBDIR and we add our own small static lib below.
FetchContent_Declare(basisu
  GIT_REPOSITORY https://github.com/BinomialLLC/basis_universal
  GIT_TAG 900e40fb5d2502927360fe2f31762bdbb624455f # 1.16.4
  SOURCE_SUBDIR cmake_unused_subdir
  SYSTEM EXCLUDE_FROM_ALL)

# --------------------------------------------------------- SheenBidi v2.9.0
# UAX#9 bidi levels for mixed LTR/RTL text (the shaper reorders runs with
# them). Upstream's build files are skipped (nonexistent SOURCE_SUBDIR, like
# basisu); a small unity-build static lib is added below.
FetchContent_Declare(sheenbidi
  GIT_REPOSITORY https://github.com/Tehreer/SheenBidi
  GIT_TAG 83f77108a2873600283f6da4b326a2dca7a3a7a6 # v2.9.0
  SOURCE_SUBDIR cmake_unused_subdir
  SYSTEM EXCLUDE_FROM_ALL)

# ------------------------------------------------------- stb (no build files)
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb
  GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
  SYSTEM EXCLUDE_FROM_ALL)

# --------------------------------------------------- dr_libs (no build files)
FetchContent_Declare(dr_libs
  GIT_REPOSITORY https://github.com/mackron/dr_libs
  GIT_TAG dfe8377631000664666519fdb83da193fd8037f4
  SYSTEM EXCLUDE_FROM_ALL)

# volk needs to be pointed at the fetched Vulkan headers (no SDK installed).
FetchContent_MakeAvailable(VulkanHeaders)
set(VULKAN_HEADERS_INSTALL_DIR "${vulkanheaders_SOURCE_DIR}" CACHE PATH "" FORCE)

FetchContent_MakeAvailable(
  fmt glm Catch2 SDL3
  volk VulkanMemoryAllocator glslang
  freetype harfbuzz yoga fastgltf draco basisu sheenbidi stb dr_libs)

# draco's CMake does not export include directories on its targets; the
# generated draco_features.h lands in ${CMAKE_BINARY_DIR}/draco/.
foreach(draco_target draco draco_static)
  if(TARGET ${draco_target})
    target_include_directories(${draco_target} SYSTEM INTERFACE
      ${draco_SOURCE_DIR}/src ${CMAKE_BINARY_DIR})
  endif()
endforeach()

# Transcoder-only basis_universal target (see the declaration above).
add_library(basisu_transcoder STATIC
  ${basisu_SOURCE_DIR}/transcoder/basisu_transcoder.cpp
  ${basisu_SOURCE_DIR}/zstd/zstd.c)
target_include_directories(basisu_transcoder SYSTEM PUBLIC
  ${basisu_SOURCE_DIR}/transcoder)
target_compile_definitions(basisu_transcoder PUBLIC
  BASISD_SUPPORT_KTX2=1 BASISD_SUPPORT_KTX2_ZSTD=1)
add_library(basisu::transcoder ALIAS basisu_transcoder)

# SheenBidi unity build (SBBase.c etc. all included by SheenBidi.c).
add_library(sheenbidi_lib STATIC ${sheenbidi_SOURCE_DIR}/Source/SheenBidi.c)
target_include_directories(sheenbidi_lib SYSTEM PUBLIC ${sheenbidi_SOURCE_DIR}/Headers)
target_compile_definitions(sheenbidi_lib PRIVATE SB_CONFIG_UNITY)
add_library(sheenbidi::sheenbidi ALIAS sheenbidi_lib)

# Header-only interface targets for the buildless deps.
add_library(stb_headers INTERFACE)
target_include_directories(stb_headers SYSTEM INTERFACE ${stb_SOURCE_DIR})
add_library(stb::stb ALIAS stb_headers)

add_library(dr_libs_headers INTERFACE)
target_include_directories(dr_libs_headers SYSTEM INTERFACE ${dr_libs_SOURCE_DIR})
add_library(dr_libs::dr_libs ALIAS dr_libs_headers)
