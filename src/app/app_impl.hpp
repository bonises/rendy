#pragma once

// The private App state shared between app.cpp and (later) Canvas/Scene glue.

#include "canvas/canvas_data.hpp"
#include "canvas/renderer2d.hpp"
#include "gpu/bindless.hpp"
#include "gpu/context.hpp"
#include "gpu/frame.hpp"
#include "gpu/swapchain.hpp"
#include "gpu/texture.hpp"
#include "gpu/upload.hpp"
#include "text/glyph_cache.hpp"
#include "rendy/app/app.hpp"
#include "rendy/canvas/canvas.hpp"
#include "rendy/scene/camera.hpp"
#include "scene/renderer3d.hpp"

#include <SDL3/SDL.h>

#include <memory>

namespace rendy::detail {

struct AppImpl {
    SDL_Window* window = nullptr;
    std::unique_ptr<gpu::Context> gpu;
    std::unique_ptr<gpu::Swapchain> swapchain;
    std::unique_ptr<gpu::FrameRing> frames;
    std::unique_ptr<gpu::BindlessTable> bindless;
    std::unique_ptr<gpu::Uploader> uploader;
    std::unique_ptr<gpu::TexturePool> textures;
    std::unique_ptr<text::GlyphCache> glyphs;
    std::unique_ptr<Renderer2D> renderer2d;
    std::unique_ptr<Renderer3D> renderer3d;
    CanvasData canvasData;

    // 3D draw request for the frame in progress.
    SceneImpl* pendingScene = nullptr;
    Camera pendingCamera;
    Color clearColor = colors::black;

    Input input;
    bool quitRequested = false;

    uint64_t lastTick = 0;   // SDL_GetTicksNS at previous pollEvents
    uint64_t startTick = 0;  // SDL_GetTicksNS at creation
    float dt = 0.0f;
    float smoothedFps = 0.0f;

    // In-flight frame state (valid between beginFrame and present).
    bool framePresented = true;
    gpu::FrameRing::FrameInfo current{};

    ~AppImpl();

    bool pollEvents();
    Frame beginFrame(const FrameConfig& config);
    void present();
    Canvas canvas() { return Canvas(&canvasData); }

#ifdef RENDY_SHADER_HOT_RELOAD
    struct WatchedShader {
        std::string path;
        std::string name; // e.g. "mesh.frag"
        uint64_t mtime = 0;
        bool isInclude = false; // .glsl: a change recompiles everything
    };
    std::vector<WatchedShader> watchedShaders;
    double lastShaderCheck = 0.0;
    void initShaderWatch();
    void checkShaderReload();
    void recompileShader(const WatchedShader& shader);
#endif
};

} // namespace rendy::detail
