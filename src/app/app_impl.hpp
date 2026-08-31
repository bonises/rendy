#pragma once

// The private App state shared between app.cpp and (later) Canvas/Scene glue.

#include "gpu/context.hpp"
#include "gpu/frame.hpp"
#include "gpu/swapchain.hpp"
#include "rendy/app/app.hpp"

#include <SDL3/SDL.h>

#include <memory>

namespace rendy::detail {

struct AppImpl {
    SDL_Window* window = nullptr;
    std::unique_ptr<gpu::Context> gpu;
    std::unique_ptr<gpu::Swapchain> swapchain;
    std::unique_ptr<gpu::FrameRing> frames;

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
};

} // namespace rendy::detail
