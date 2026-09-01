#pragma once

// In-process GLSL → SPIR-V compilation for shader hot reload. Only does
// anything when built with RENDY_SHADER_HOT_RELOAD (glslang linked in);
// otherwise compileGlsl reports unavailability.

#include "rendy/core/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace rendy::gpu {

/// True when this build can compile shaders at runtime.
bool shaderCompilerAvailable();

/// Compiles a .vert/.frag/.comp file (stage from the extension). #include
/// resolves relative to the file's directory.
Result<std::vector<uint32_t>> compileGlslFile(const std::string& path);

} // namespace rendy::gpu
