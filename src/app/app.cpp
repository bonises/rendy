#include "app/app_impl.hpp"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cmath>

#ifdef RENDY_SHADER_HOT_RELOAD
#include "gpu/shader_compiler.hpp"

#include <filesystem>
#endif

namespace rendy {
namespace detail {
namespace {

Key mapKey(SDL_Scancode code) {
    // clang-format off
    switch (code) {
    case SDL_SCANCODE_A: return Key::A; case SDL_SCANCODE_B: return Key::B;
    case SDL_SCANCODE_C: return Key::C; case SDL_SCANCODE_D: return Key::D;
    case SDL_SCANCODE_E: return Key::E; case SDL_SCANCODE_F: return Key::F;
    case SDL_SCANCODE_G: return Key::G; case SDL_SCANCODE_H: return Key::H;
    case SDL_SCANCODE_I: return Key::I; case SDL_SCANCODE_J: return Key::J;
    case SDL_SCANCODE_K: return Key::K; case SDL_SCANCODE_L: return Key::L;
    case SDL_SCANCODE_M: return Key::M; case SDL_SCANCODE_N: return Key::N;
    case SDL_SCANCODE_O: return Key::O; case SDL_SCANCODE_P: return Key::P;
    case SDL_SCANCODE_Q: return Key::Q; case SDL_SCANCODE_R: return Key::R;
    case SDL_SCANCODE_S: return Key::S; case SDL_SCANCODE_T: return Key::T;
    case SDL_SCANCODE_U: return Key::U; case SDL_SCANCODE_V: return Key::V;
    case SDL_SCANCODE_W: return Key::W; case SDL_SCANCODE_X: return Key::X;
    case SDL_SCANCODE_Y: return Key::Y; case SDL_SCANCODE_Z: return Key::Z;
    case SDL_SCANCODE_1: return Key::Num1; case SDL_SCANCODE_2: return Key::Num2;
    case SDL_SCANCODE_3: return Key::Num3; case SDL_SCANCODE_4: return Key::Num4;
    case SDL_SCANCODE_5: return Key::Num5; case SDL_SCANCODE_6: return Key::Num6;
    case SDL_SCANCODE_7: return Key::Num7; case SDL_SCANCODE_8: return Key::Num8;
    case SDL_SCANCODE_9: return Key::Num9; case SDL_SCANCODE_0: return Key::Num0;
    case SDL_SCANCODE_F1: return Key::F1; case SDL_SCANCODE_F2: return Key::F2;
    case SDL_SCANCODE_F3: return Key::F3; case SDL_SCANCODE_F4: return Key::F4;
    case SDL_SCANCODE_F5: return Key::F5; case SDL_SCANCODE_F6: return Key::F6;
    case SDL_SCANCODE_F7: return Key::F7; case SDL_SCANCODE_F8: return Key::F8;
    case SDL_SCANCODE_F9: return Key::F9; case SDL_SCANCODE_F10: return Key::F10;
    case SDL_SCANCODE_F11: return Key::F11; case SDL_SCANCODE_F12: return Key::F12;
    case SDL_SCANCODE_ESCAPE: return Key::Escape;
    case SDL_SCANCODE_RETURN: return Key::Enter;
    case SDL_SCANCODE_TAB: return Key::Tab;
    case SDL_SCANCODE_BACKSPACE: return Key::Backspace;
    case SDL_SCANCODE_SPACE: return Key::Space;
    case SDL_SCANCODE_INSERT: return Key::Insert;
    case SDL_SCANCODE_DELETE: return Key::Delete;
    case SDL_SCANCODE_HOME: return Key::Home;
    case SDL_SCANCODE_END: return Key::End;
    case SDL_SCANCODE_PAGEUP: return Key::PageUp;
    case SDL_SCANCODE_PAGEDOWN: return Key::PageDown;
    case SDL_SCANCODE_LEFT: return Key::Left;
    case SDL_SCANCODE_RIGHT: return Key::Right;
    case SDL_SCANCODE_UP: return Key::Up;
    case SDL_SCANCODE_DOWN: return Key::Down;
    case SDL_SCANCODE_LSHIFT: return Key::LeftShift;
    case SDL_SCANCODE_RSHIFT: return Key::RightShift;
    case SDL_SCANCODE_LCTRL: return Key::LeftCtrl;
    case SDL_SCANCODE_RCTRL: return Key::RightCtrl;
    case SDL_SCANCODE_LALT: return Key::LeftAlt;
    case SDL_SCANCODE_RALT: return Key::RightAlt;
    case SDL_SCANCODE_LGUI: case SDL_SCANCODE_RGUI: return Key::Super;
    case SDL_SCANCODE_MINUS: return Key::Minus;
    case SDL_SCANCODE_EQUALS: return Key::Equals;
    case SDL_SCANCODE_LEFTBRACKET: return Key::LeftBracket;
    case SDL_SCANCODE_RIGHTBRACKET: return Key::RightBracket;
    case SDL_SCANCODE_BACKSLASH: return Key::Backslash;
    case SDL_SCANCODE_SEMICOLON: return Key::Semicolon;
    case SDL_SCANCODE_APOSTROPHE: return Key::Apostrophe;
    case SDL_SCANCODE_GRAVE: return Key::Grave;
    case SDL_SCANCODE_COMMA: return Key::Comma;
    case SDL_SCANCODE_PERIOD: return Key::Period;
    case SDL_SCANCODE_SLASH: return Key::Slash;
    default: return Key::Unknown;
    }
    // clang-format on
}

int mapMouseButton(Uint8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return static_cast<int>(MouseButton::Left);
    case SDL_BUTTON_RIGHT: return static_cast<int>(MouseButton::Right);
    case SDL_BUTTON_MIDDLE: return static_cast<int>(MouseButton::Middle);
    default: return -1;
    }
}

} // namespace

AppImpl::~AppImpl() {
    // Tear down strictly in reverse creation order; everything owning GPU
    // resources must die before the allocator/device in `gpu`.
    if (gpu) gpu->waitIdle();
    if (captureBuffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(gpu->allocator(), captureBuffer, captureAllocation);
    renderer3d.reset();
    renderer2d.reset();
    glyphs.reset();
    textures.reset();
    uploader.reset();
    bindless.reset();
    frames.reset();
    swapchain.reset();
    gpu.reset();
    if (window != nullptr) SDL_DestroyWindow(window);
    SDL_Quit();
}

#ifdef RENDY_SHADER_HOT_RELOAD

namespace {
uint64_t mtimeOf(const std::filesystem::path& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    return ec ? 0 : static_cast<uint64_t>(time.time_since_epoch().count());
}
} // namespace

void AppImpl::initShaderWatch() {
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(RENDY_SHADER_SOURCE_DIR, ec)) {
        const auto extension = entry.path().extension().string();
        const bool isStage = extension == ".vert" || extension == ".frag";
        const bool isInclude = extension == ".glsl";
        if (!isStage && !isInclude) continue;
        watchedShaders.push_back({entry.path().string(), entry.path().filename().string(),
                                  mtimeOf(entry.path()), isInclude});
    }
    if (!watchedShaders.empty())
        log::debug("shader hot reload: watching {} files in {}", watchedShaders.size(),
                   RENDY_SHADER_SOURCE_DIR);
}

void AppImpl::recompileShader(const WatchedShader& shader) {
    auto spirv = gpu::compileGlslFile(shader.path);
    if (!spirv) {
        log::error("{}", spirv.error().message);
        return; // keep the old pipeline
    }
    bool used = renderer2d->reloadShader(shader.name, spirv.value(), *frames);
    if (!used) used = renderer3d->reloadShader(shader.name, std::move(spirv).value(), *frames);
    if (used) log::info("shader hot reload: {} rebuilt", shader.name);
}

void AppImpl::checkShaderReload() {
    const double now = static_cast<double>(SDL_GetTicksNS()) * 1e-9;
    if (now - lastShaderCheck < 0.5) return;
    lastShaderCheck = now;

    bool includeChanged = false;
    std::vector<const WatchedShader*> changed;
    for (WatchedShader& shader : watchedShaders) {
        const uint64_t mtime = mtimeOf(shader.path);
        if (mtime == 0 || mtime == shader.mtime) continue;
        shader.mtime = mtime;
        if (shader.isInclude)
            includeChanged = true;
        else
            changed.push_back(&shader);
    }
    if (includeChanged) {
        // A shared include changed: recompile every stage that may use it.
        for (const WatchedShader& shader : watchedShaders)
            if (!shader.isInclude) recompileShader(shader);
    } else {
        for (const WatchedShader* shader : changed) recompileShader(*shader);
    }
}

#endif // RENDY_SHADER_HOT_RELOAD

bool AppImpl::pollEvents() {
#ifdef RENDY_SHADER_HOT_RELOAD
    checkShaderReload();
#endif
    // Edge states last exactly one pollEvents interval.
    input.mousePressed_.reset();
    input.mouseReleased_.reset();
    input.keyPressed_.reset();
    input.keyReleased_.reset();
    input.wheel_ = Vec2{0.0f};
    input.mouseDelta_ = Vec2{0.0f};
    input.text_.clear();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            quitRequested = true;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            input.mousePos_ = {event.motion.x, event.motion.y};
            input.mouseDelta_ += Vec2{event.motion.xrel, event.motion.yrel};
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const int index = mapMouseButton(event.button.button);
            if (index >= 0) {
                const auto i = static_cast<size_t>(index);
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    input.mouseDown_[i] = true;
                    input.mousePressed_[i] = true;
                } else {
                    input.mouseDown_[i] = false;
                    input.mouseReleased_[i] = true;
                }
            }
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            input.wheel_ += Vec2{event.wheel.x, event.wheel.y};
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const Key key = mapKey(event.key.scancode);
            if (key != Key::Unknown) {
                const size_t i = Input::idx(key);
                if (event.type == SDL_EVENT_KEY_DOWN) {
                    input.keyDown_[i] = true;
                    input.keyPressed_[i] = true; // includes OS key-repeat
                } else {
                    input.keyDown_[i] = false;
                    input.keyReleased_[i] = true;
                }
            }
            break;
        }
        case SDL_EVENT_TEXT_INPUT:
            input.text_ += event.text.text;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            // The WSI won't always report OUT_OF_DATE on resize; recreate
            // proactively at the next beginFrame.
            if (swapchain) swapchain->markStale();
            break;
        default:
            break;
        }
    }

    const uint64_t now = SDL_GetTicksNS();
    dt = std::min(static_cast<float>(now - lastTick) * 1e-9f, 0.1f);
    lastTick = now;
    if (dt > 0.0f) {
        const float instantFps = 1.0f / dt;
        smoothedFps = smoothedFps == 0.0f ? instantFps
                                          : smoothedFps + 0.05f * (instantFps - smoothedFps);
    }

    return !quitRequested;
}

Frame AppImpl::beginFrame(const FrameConfig& config) {
    current = frames->begin();
    framePresented = false;
    pendingScene = nullptr;
    clearColor = config.clear;
    const VkExtent2D extent = swapchain->extent();
    canvasData.reset(
        {static_cast<float>(extent.width), static_cast<float>(extent.height)});
    return Frame(this);
}

void AppImpl::present() {
    if (framePresented) return;
    framePresented = true;

    if (current.ok) {
        glyphs->flushUploads();

        gpu::imageBarrier(current.cmd, swapchain->image(current.imageIndex),
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        // 3D first: scene pass → resolve → tonemap fills the swapchain image.
        const bool has3d = pendingScene != nullptr;
        if (has3d)
            renderer3d->render(current.cmd, frames->slot(), *pendingScene, pendingCamera,
                               swapchain->extent(), swapchain->imageView(current.imageIndex),
                               *frames);

        // 2D pass on top (clears when there was no 3D underneath).
        auto toLinear = [](float srgb) {
            return srgb <= 0.04045f ? srgb / 12.92f
                                    : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
        };
        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = swapchain->imageView(current.imageIndex);
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = has3d ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = {{toLinear(clearColor.r), toLinear(clearColor.g),
                                             toLinear(clearColor.b), clearColor.a}};

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = {{0, 0}, swapchain->extent()};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        vkCmdBeginRendering(current.cmd, &renderingInfo);
        renderer2d->flush(current.cmd, frames->slot(), canvasData, *frames);
        vkCmdEndRendering(current.cmd);

        if (captureRequested && !swapchain->captureSupported()) {
            // TRANSFER_SRC on swapchain images (or an 8-bit sRGB format)
            // is optional in Vulkan — degrade to a plain error.
            log::warn("screenshot: this surface doesn't support readback");
            captureRequested = false;
        }
        if (captureRequested) {
            // Screenshot: copy the finished frame into a host-visible
            // buffer on its way to present.
            const VkExtent2D extent = swapchain->extent();
            const size_t bytes = static_cast<size_t>(extent.width) * extent.height * 4;
            if (bytes > captureBufferBytes) {
                if (captureBuffer != VK_NULL_HANDLE)
                    vmaDestroyBuffer(gpu->allocator(), captureBuffer, captureAllocation);
                VkBufferCreateInfo bufferInfo{};
                bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bufferInfo.size = bytes;
                bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                VmaAllocationCreateInfo allocCreate{};
                allocCreate.usage = VMA_MEMORY_USAGE_AUTO;
                allocCreate.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo allocInfo{};
                VK_CHECK(vmaCreateBuffer(gpu->allocator(), &bufferInfo, &allocCreate,
                                         &captureBuffer, &captureAllocation, &allocInfo));
                captureMapped = allocInfo.pMappedData;
                captureBufferBytes = bytes;
            }
            gpu::imageBarrier(current.cmd, swapchain->image(current.imageIndex),
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(current.cmd, swapchain->image(current.imageIndex),
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuffer, 1,
                                   &copy);
            gpu::imageBarrier(current.cmd, swapchain->image(current.imageIndex),
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                              VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                              VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
        } else {
            gpu::imageBarrier(current.cmd, swapchain->image(current.imageIndex),
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
        }
    }
    frames->end();

    if (captureRequested && current.ok) {
        // Blocking read-out; screenshots are an explicitly slow path.
        gpu->waitIdle();
        vmaInvalidateAllocation(gpu->allocator(), captureAllocation, 0, VK_WHOLE_SIZE);
        const VkExtent2D extent = swapchain->extent();
        capture.size = {static_cast<int>(extent.width), static_cast<int>(extent.height)};
        capture.rgba.resize(static_cast<size_t>(extent.width) * extent.height * 4);
        const auto* src = static_cast<const uint8_t*>(captureMapped);
        if (swapchain->format() == VK_FORMAT_B8G8R8A8_SRGB) {
            for (size_t i = 0; i < capture.rgba.size(); i += 4) { // BGRA → RGBA
                capture.rgba[i + 0] = src[i + 2];
                capture.rgba[i + 1] = src[i + 1];
                capture.rgba[i + 2] = src[i + 0];
                capture.rgba[i + 3] = src[i + 3];
            }
        } else { // R8G8B8A8_SRGB — captureSupported() allows no other formats
            std::memcpy(capture.rgba.data(), src, capture.rgba.size());
        }
        captureRequested = false;
        captureReady = true;
    }
}

} // namespace detail

// ------------------------------------------------------------------- Frame

Frame::Frame(Frame&& other) noexcept : app_(other.app_) { other.app_ = nullptr; }

Frame::~Frame() {
    if (app_ != nullptr) app_->present();
}

void Frame::present() {
    if (app_ != nullptr) {
        app_->present();
        app_ = nullptr;
    }
}

Canvas Frame::canvas() { return app_->canvas(); }

void Frame::draw(Scene& scene, const Camera& camera) {
    app_->pendingScene = scene.impl_.get();
    app_->pendingCamera = camera;
}

// --------------------------------------------------------------------- App

Result<App> App::create(const AppConfig& config) {
    if (!SDL_Init(SDL_INIT_VIDEO))
        return err("SDL_Init failed: {}", SDL_GetError());

    auto impl = std::make_unique<detail::AppImpl>();

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (config.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (config.hidden) flags |= SDL_WINDOW_HIDDEN;
    impl->window = SDL_CreateWindow(config.title.c_str(), config.size.x, config.size.y, flags);
    if (impl->window == nullptr)
        return err("SDL_CreateWindow failed: {}", SDL_GetError());

    Uint32 extensionCount = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    if (extensions == nullptr)
        return err("SDL_Vulkan_GetInstanceExtensions failed: {}", SDL_GetError());

    gpu::ContextConfig gpuConfig;
#ifdef RENDY_ENABLE_VALIDATION
    gpuConfig.validation = true;
#endif
    gpuConfig.validation = gpuConfig.validation || config.validation;
    gpuConfig.instanceExtensions.assign(extensions, extensions + extensionCount);

    auto context = gpu::Context::create(gpuConfig);
    if (!context) return context.error();
    impl->gpu = std::move(context).value();

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(impl->window, impl->gpu->instance(), nullptr, &surface))
        return err("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());

    impl->swapchain = std::make_unique<gpu::Swapchain>(*impl->gpu, surface, config.vsync);
    impl->frames = std::make_unique<gpu::FrameRing>(*impl->gpu, *impl->swapchain);
    impl->bindless = std::make_unique<gpu::BindlessTable>(*impl->gpu);
    impl->uploader = std::make_unique<gpu::Uploader>(*impl->gpu);
    impl->textures =
        std::make_unique<gpu::TexturePool>(*impl->gpu, *impl->bindless, *impl->uploader);
    impl->glyphs = std::make_unique<text::GlyphCache>(*impl->textures);
    impl->glyphs->loadDefaultFonts();
    impl->canvasData.glyphCache = impl->glyphs.get();
    impl->renderer2d = std::make_unique<detail::Renderer2D>(*impl->gpu, *impl->bindless,
                                                            impl->swapchain->format());
    impl->renderer3d = std::make_unique<detail::Renderer3D>(
        *impl->gpu, *impl->bindless, *impl->uploader, impl->swapchain->format());
#ifdef RENDY_SHADER_HOT_RELOAD
    impl->initShaderWatch();
#endif

    impl->startTick = impl->lastTick = SDL_GetTicksNS();
    SDL_StartTextInput(impl->window);

    return App(std::move(impl));
}

App::App(std::unique_ptr<detail::AppImpl> impl) : impl_(std::move(impl)) {}
App::App(App&&) noexcept = default;
App& App::operator=(App&&) noexcept = default;
App::~App() = default;

bool App::pollEvents() { return impl_->pollEvents(); }

Result<TextureRef> App::loadTexture(const std::string& path, const TextureOptions& options) {
    return impl_->textures->loadFromFile(path, options);
}

Result<TextureRef> App::createTexture(const void* rgbaPixels, IVec2 size,
                                      const TextureOptions& options) {
    return impl_->textures->createFromPixels(rgbaPixels, size, options);
}

Result<FontRef> App::loadFont(const std::string& path) {
    return impl_->glyphs->loadFont(path);
}

FontRef App::defaultFont() const { return FontRef{0}; }

void App::destroyTexture(TextureRef texture) {
    gpu::TexturePool* pool = impl_->textures.get();
    impl_->frames->defer([pool, texture] { pool->destroy(texture); });
}
void App::quit() { impl_->quitRequested = true; }
Frame App::beginFrame(const FrameConfig& config) { return impl_->beginFrame(config); }
void App::requestScreenshot() { impl_->captureRequested = true; }

Result<Screenshot> App::takeScreenshot() {
    if (!impl_->captureReady)
        return err("no screenshot captured — requestScreenshot() then present a frame");
    impl_->captureReady = false;
    return std::move(impl_->capture);
}

const Input& App::input() const { return impl_->input; }

IVec2 App::pixelSize() const {
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(impl_->window, &w, &h);
    return {w, h};
}

float App::dt() const { return impl_->dt; }
double App::time() const {
    return static_cast<double>(SDL_GetTicksNS() - impl_->startTick) * 1e-9;
}
float App::fps() const { return impl_->smoothedFps; }

} // namespace rendy
