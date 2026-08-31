#pragma once

/// \file app.hpp
/// App owns the window, the GPU, input, and frame pacing. The main loop is
/// yours:
///
///     auto app = rendy::App::create({.title = "hi"}).value();
///     while (app.pollEvents()) {
///         auto frame = app.beginFrame({.clear = rendy::colors::slate});
///         // draw...
///         frame.present();
///     }

#include "../core/color.hpp"
#include "../core/result.hpp"
#include "../math/math.hpp"
#include "input.hpp"

#include <memory>
#include <string>

namespace rendy {

namespace detail {
struct AppImpl;
}

struct AppConfig {
    std::string title = "rendy";
    IVec2 size{1280, 720}; ///< initial window size (logical units)
    bool resizable = true;
    bool vsync = true;
    bool validation = false; ///< force Vulkan validation (also RENDY_VULKAN_VALIDATION build opt)
};

struct FrameConfig {
    Color clear = colors::black;
};

/// One in-progress frame. Obtained from App::beginFrame(); presenting (or
/// destroying) it submits the frame. Not movable across frames — use and drop.
class Frame {
public:
    Frame(Frame&& other) noexcept;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame& operator=(Frame&&) = delete;
    ~Frame(); ///< presents if present() wasn't called

    /// Submit and present this frame. Idempotent.
    void present();

private:
    friend struct detail::AppImpl;
    explicit Frame(detail::AppImpl* app) : app_(app) {}
    detail::AppImpl* app_;
};

class App {
public:
    static Result<App> create(const AppConfig& config = {});
    App(App&&) noexcept;
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App& operator=(App&&) noexcept;
    ~App();

    /// Pump OS events and refresh input(). Returns false once the user closes
    /// the window (or after quit() was called).
    bool pollEvents();

    /// Request that pollEvents() returns false next time.
    void quit();

    Frame beginFrame(const FrameConfig& config = {});

    [[nodiscard]] const Input& input() const;
    /// Framebuffer size in pixels (drawing coordinate space).
    [[nodiscard]] IVec2 pixelSize() const;
    /// Seconds since the previous pollEvents(), clamped to 0.1 s.
    [[nodiscard]] float dt() const;
    /// Seconds since App creation.
    [[nodiscard]] double time() const;
    /// Frames per second, smoothed.
    [[nodiscard]] float fps() const;

private:
    explicit App(std::unique_ptr<detail::AppImpl> impl);
    std::unique_ptr<detail::AppImpl> impl_;
};

} // namespace rendy
