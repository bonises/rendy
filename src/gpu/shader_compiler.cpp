#include "gpu/shader_compiler.hpp"

#include "rendy/core/log.hpp"

#ifdef RENDY_SHADER_HOT_RELOAD

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace rendy::gpu {
namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Resolves #include "..." relative to the including file's directory.
class FileIncluder : public glslang::TShader::Includer {
public:
    explicit FileIncluder(std::filesystem::path directory)
        : directory_(std::move(directory)) {}

    IncludeResult* includeLocal(const char* headerName, const char*, size_t) override {
        const auto path = directory_ / headerName;
        if (!std::filesystem::exists(path)) return nullptr;
        auto* content = new std::string(readFile(path));
        return new IncludeResult(headerName, content->c_str(), content->size(), content);
    }
    IncludeResult* includeSystem(const char*, const char*, size_t) override { return nullptr; }
    void releaseInclude(IncludeResult* result) override {
        if (result == nullptr) return;
        delete static_cast<std::string*>(result->userData);
        delete result;
    }

private:
    std::filesystem::path directory_;
};

EShLanguage stageFor(const std::string& extension) {
    if (extension == ".vert") return EShLangVertex;
    if (extension == ".frag") return EShLangFragment;
    if (extension == ".comp") return EShLangCompute;
    return EShLangCount;
}

std::once_flag g_initFlag;

} // namespace

bool shaderCompilerAvailable() { return true; }

Result<std::vector<uint32_t>> compileGlslFile(const std::string& pathText) {
    std::call_once(g_initFlag, [] { glslang::InitializeProcess(); });

    const std::filesystem::path path(pathText);
    const EShLanguage stage = stageFor(path.extension().string());
    if (stage == EShLangCount)
        return err("shader: unknown stage for '{}'", pathText);

    const std::string source = readFile(path);
    if (source.empty()) return err("shader: cannot read '{}'", pathText);

    glslang::TShader shader(stage);
    const char* sourcePtr = source.c_str();
    shader.setStrings(&sourcePtr, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);

    FileIncluder includer(path.parent_path());
    if (!shader.parse(GetDefaultResources(), 460, false, EShMsgDefault, includer))
        return err("shader: {} failed to compile:\n{}", pathText, shader.getInfoLog());

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault))
        return err("shader: {} failed to link:\n{}", pathText, program.getInfoLog());

    std::vector<uint32_t> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);
    if (spirv.empty()) return err("shader: {} produced no SPIR-V", pathText);
    return spirv;
}

} // namespace rendy::gpu

#else // !RENDY_SHADER_HOT_RELOAD

namespace rendy::gpu {

bool shaderCompilerAvailable() { return false; }

Result<std::vector<uint32_t>> compileGlslFile(const std::string& path) {
    return err("shader hot reload not compiled in (RENDY_SHADER_HOT_RELOAD=OFF): {}", path);
}

} // namespace rendy::gpu

#endif
