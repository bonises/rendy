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

#include "../canvas/canvas.hpp"
#include "../core/color.hpp"
#include "../core/result.hpp"
#include "../gpu/texture.hpp"
#include "../math/math.hpp"
#include "input.hpp"

#include <memory>
#include <string>
#include <vector>

namespace rendy {

namespace detail {
struct AppImpl;
}
namespace ui {
class Context;
}
class Scene;
struct Camera;

struct AppConfig {
    std::string title = "rendy";
    IVec2 size{1280, 720}; ///< initial window size (logical units)
    bool resizable = true;
    bool vsync = true;
    bool validation = false; ///< force Vulkan validation (also RENDY_VULKAN_VALIDATION build opt)
    bool hidden = false;     ///< create the window hidden (offscreen tests/tools)
};

/// A captured frame (see App::requestScreenshot).
struct Screenshot {
    IVec2 size{0, 0};
    std::vector<uint8_t> rgba; ///< 8-bit sRGB RGBA, row-major, top-left origin
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

    /// The 2D drawing surface for this frame. Drawn on top of any 3D scene.
    [[nodiscard]] Canvas canvas();

    /// Render a 3D scene with lights, shadows and tonemapping. At most one
    /// scene per frame; call before present().
    void draw(Scene& scene, const Camera& camera);

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

    /// Load an image file (PNG/JPEG/BMP/TGA/HDR...) into a GPU texture.
    Result<TextureRef> loadTexture(const std::string& path, const TextureOptions& options = {});
    /// Create a texture from tightly packed RGBA8 pixels.
    Result<TextureRef> createTexture(const void* rgbaPixels, IVec2 size,
                                     const TextureOptions& options = {});
    /// Destroy a texture. Safe while frames using it are still in flight.
    void destroyTexture(TextureRef texture);

    /// Load a .ttf/.otf font.
    Result<FontRef> loadFont(const std::string& path);
    /// The system sans-serif found at startup (id 0).
    [[nodiscard]] FontRef defaultFont() const;

    /// Capture the NEXT presented frame's pixels (that present blocks
    /// briefly). Fetch the result with takeScreenshot() afterwards.
    void requestScreenshot();
    /// The frame captured after the latest requestScreenshot(), or an error
    /// when no capture has completed. Consumes the capture.
    Result<Screenshot> takeScreenshot();

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
    friend class ui::Context;
    friend class Scene;
    explicit App(std::unique_ptr<detail::AppImpl> impl);
    std::unique_ptr<detail::AppImpl> impl_;
};

} // namespace rendy
