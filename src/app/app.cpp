#include "app/app_impl.hpp"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cmath>

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
    frames.reset();
    swapchain.reset();
    gpu.reset();
    if (window != nullptr) SDL_DestroyWindow(window);
    SDL_Quit();
}

bool AppImpl::pollEvents() {
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

    if (current.ok) {
        const Color c = config.clear;
        gpu::imageBarrier(current.cmd, swapchain->image(current.imageIndex),
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        // The swapchain is sRGB: attachment clears want linear values.
        auto toLinear = [](float srgb) {
            return srgb <= 0.04045f ? srgb / 12.92f
                                    : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
        };
        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = swapchain->imageView(current.imageIndex);
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = {{toLinear(c.r), toLinear(c.g), toLinear(c.b), c.a}};

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = {{0, 0}, swapchain->extent()};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        vkCmdBeginRendering(current.cmd, &renderingInfo);
    }

    return Frame(this);
}

void AppImpl::present() {
    if (framePresented) return;
    framePresented = true;

    if (current.ok) {
        vkCmdEndRendering(current.cmd);
        gpu::imageBarrier(current.cmd, swapchain->image(current.imageIndex),
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    }
    frames->end();
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

// --------------------------------------------------------------------- App

Result<App> App::create(const AppConfig& config) {
    if (!SDL_Init(SDL_INIT_VIDEO))
        return err("SDL_Init failed: {}", SDL_GetError());

    auto impl = std::make_unique<detail::AppImpl>();

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (config.resizable) flags |= SDL_WINDOW_RESIZABLE;
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

    impl->startTick = impl->lastTick = SDL_GetTicksNS();
    SDL_StartTextInput(impl->window);

    return App(std::move(impl));
}

App::App(std::unique_ptr<detail::AppImpl> impl) : impl_(std::move(impl)) {}
App::App(App&&) noexcept = default;
App& App::operator=(App&&) noexcept = default;
App::~App() = default;

bool App::pollEvents() { return impl_->pollEvents(); }
void App::quit() { impl_->quitRequested = true; }
Frame App::beginFrame(const FrameConfig& config) { return impl_->beginFrame(config); }
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
