#include "rendy/ui/ui.hpp"

#include "app/app_impl.hpp"
#include "css/cascade.hpp"
#include "css/parser.hpp"
#include "ui/yoga_layout.hpp"

#include <yoga/Yoga.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace rendy::ui {
namespace detail {

struct Node {
    ContextImpl* ctx = nullptr;
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;

    std::string tag;
    std::string id;
    std::vector<std::string> classes;
    std::string text;
    std::vector<Declaration> inlineStyle;
    std::function<void(Element)> onClick;

    YGNodeRef yoga = nullptr;
    css::ComputedStyle style;
    Rect rect;               ///< absolute px, after layout
    Vec2 scrollOffset{0.0f};
    Vec2 contentSize{0.0f};  ///< children extent for scrolling
    uint8_t pseudo = 0;      ///< css::PseudoFlags

    ~Node() {
        children.clear(); // children free their yoga nodes first
        if (yoga != nullptr) YGNodeFree(yoga);
    }
};

struct ContextImpl {
    rendy::detail::AppImpl* app = nullptr;
    YGConfigRef yogaConfig = nullptr;
    std::unique_ptr<Node> root;
    bool dirty = true;

    std::vector<css::Stylesheet> sheets;
    struct WatchedFile {
        std::string path;
        size_t sheetIndex;
        std::filesystem::file_time_type mtime;
    };
    std::vector<WatchedFile> watched;
    double lastWatchCheck = 0.0;

    std::unordered_map<std::string, FontRef> fonts;

    Node* hovered = nullptr;
    Node* pressed = nullptr;
    Node* focused = nullptr;

    // Rebuilt once per restyle pass, not once per element.
    std::vector<const css::Stylesheet*> sheetPtrs;

    void restyle() {
        sheetPtrs.clear();
        sheetPtrs.reserve(sheets.size());
        for (const auto& sheet : sheets) sheetPtrs.push_back(&sheet);
        restyleTree(root.get(), nullptr, nullptr);
    }

    // ------------------------------------------------------------- helpers

    Node* newNode(std::string_view tag) {
        auto node = std::make_unique<Node>();
        node->ctx = this;
        node->tag = tag;
        node->yoga = YGNodeNewWithConfig(yogaConfig);
        YGNodeSetContext(node->yoga, node.get());
        Node* raw = node.get();
        raw->children.reserve(4);
        looseNodes.push_back(std::move(node));
        return raw;
    }
    // Ownership bookkeeping: nodes live in parent->children; newNode parks
    // them here until attached.
    std::vector<std::unique_ptr<Node>> looseNodes;

    std::unique_ptr<Node> takeLoose(Node* node) {
        for (auto& loose : looseNodes) {
            if (loose.get() == node) {
                auto owned = std::move(loose);
                looseNodes.erase(looseNodes.begin() + (&loose - looseNodes.data()));
                return owned;
            }
        }
        return nullptr;
    }

    void markDirty() { dirty = true; }

    void forgetNode(Node* node) {
        if (hovered == node) hovered = nullptr;
        if (pressed == node) pressed = nullptr;
        if (focused == node) focused = nullptr;
        for (auto& child : node->children) forgetNode(child.get());
    }

    FontRef resolveFont(const std::string& family) const {
        if (!family.empty())
            if (auto it = fonts.find(family); it != fonts.end()) return it->second;
        return FontRef{0};
    }

    // -------------------------------------------------------------- events

    Node* hitTest(Node* node, Vec2 point) {
        if (node->style.display == Display::None) return nullptr;
        const bool inside = node->rect.contains(point);
        // A clipping node rejects the whole subtree when the point is outside.
        if (!inside && node->style.overflow != Overflow::Visible) return nullptr;
        // Children on top, last drawn wins.
        for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
            if (Node* hit = hitTest(it->get(), point)) return hit;
        }
        return inside ? node : nullptr;
    }

    Node* scrollTarget(Node* node) {
        for (Node* cur = node; cur != nullptr; cur = cur->parent) {
            if (cur->style.overflow == Overflow::Scroll &&
                cur->contentSize.y > cur->rect.size.y + 0.5f)
                return cur;
        }
        return nullptr;
    }

    void processInput() {
        const Input& input = app->input;
        Node* hit = root ? hitTest(root.get(), input.mousePos()) : nullptr;

        if (hit != hovered) {
            setPseudo(hovered, css::kPseudoHover, false);
            hovered = hit;
            setPseudo(hovered, css::kPseudoHover, true);
        }
        if (input.mousePressed(MouseButton::Left)) {
            pressed = hit;
            setPseudo(pressed, css::kPseudoActive, true);
            if (focused != hit) {
                setPseudo(focused, css::kPseudoFocus, false);
                focused = hit;
                setPseudo(focused, css::kPseudoFocus, true);
            }
        }
        if (input.wheel().y != 0.0f && hit != nullptr) {
            if (Node* scroller = scrollTarget(hit)) {
                scroller->scrollOffset.y -= input.wheel().y * 48.0f;
                const float maxScroll =
                    std::max(0.0f, scroller->contentSize.y - scroller->rect.size.y);
                scroller->scrollOffset.y = std::clamp(scroller->scrollOffset.y, 0.0f, maxScroll);
                markDirty(); // rects move; recompute
            }
        }
        // Click handlers go LAST: they may remove nodes, which would leave
        // `hit` (and anything derived from it) dangling for later steps.
        if (input.mouseReleased(MouseButton::Left)) {
            setPseudo(pressed, css::kPseudoActive, false);
            if (pressed != nullptr && pressed == hit &&
                (pressed->pseudo & css::kPseudoDisabled) == 0) {
                // Fire on the pressed element or its closest clickable parent.
                for (Node* cur = pressed; cur != nullptr; cur = cur->parent) {
                    if (cur->onClick) {
                        cur->onClick(Element(cur));
                        break;
                    }
                }
            }
            pressed = nullptr;
        }
    }

    void setPseudo(Node* node, uint8_t flag, bool on) {
        if (node == nullptr) return;
        const uint8_t before = node->pseudo;
        if (on)
            node->pseudo |= flag;
        else
            node->pseudo &= static_cast<uint8_t>(~flag);
        if (node->pseudo != before) markDirty();
    }

    // --------------------------------------------------------------- style

    void restyleTree(Node* node, const css::MatchContext* parentMatch,
                     const css::ComputedStyle* parentStyle) {
        css::MatchContext match;
        match.tag = node->tag;
        match.id = node->id;
        match.classes = &node->classes;
        // first/last-child are structural pseudo states.
        uint8_t structural = 0;
        if (node->parent != nullptr) {
            if (node->parent->children.front().get() == node)
                structural |= css::kPseudoFirstChild;
            if (node->parent->children.back().get() == node)
                structural |= css::kPseudoLastChild;
        }
        match.pseudo = node->pseudo | structural;
        match.parent = parentMatch;

        css::ComputedStyle style; // defaults
        if (parentStyle != nullptr) {
            style.textColor = parentStyle->textColor;
            style.fontSize = parentStyle->fontSize;
            style.fontFamily = parentStyle->fontFamily;
            style.textAlign = parentStyle->textAlign;
            style.lineHeight = parentStyle->lineHeight;
        }

        css::resolveStyle(sheetPtrs, match, &node->inlineStyle, &style);
        node->style = std::move(style);

        applyStyleToYoga(node->style, node->yoga);
        syncMeasure(node);

        for (auto& child : node->children)
            restyleTree(child.get(), &match, &node->style);
    }

    static YGSize measureText(YGNodeConstRef yogaNode, float width, YGMeasureMode widthMode,
                              float, YGMeasureMode) {
        auto* node = static_cast<Node*>(YGNodeGetContext(yogaNode));
        ContextImpl* self = node->ctx;
        text::GlyphCache* glyphs = self->app->glyphs.get();
        const FontRef font = self->resolveFont(node->style.fontFamily);
        if (glyphs == nullptr || !glyphs->hasFont(font.id)) return {0.0f, 0.0f};

        // No wrapping in v1: measure the natural size.
        Canvas canvas = self->app->canvas();
        const Vec2 size = canvas.measureText(
            node->text, {.font = font, .size = node->style.fontSize});
        float w = size.x;
        if (widthMode == YGMeasureModeAtMost) w = std::min(w, width);
        float h = size.y;
        if (node->style.lineHeight > 0.0f)
            h = h / glyphs->metrics(font.id, node->style.fontSize).lineHeight *
                (node->style.lineHeight * node->style.fontSize);
        else if (node->style.lineHeight < 0.0f)
            h = -node->style.lineHeight;
        return {w, h};
    }

    void syncMeasure(Node* node) {
        const bool wantsMeasure = node->children.empty() && !node->text.empty();
        const bool hasMeasure = YGNodeHasMeasureFunc(node->yoga);
        if (wantsMeasure && !hasMeasure) YGNodeSetMeasureFunc(node->yoga, measureText);
        if (!wantsMeasure && hasMeasure) YGNodeSetMeasureFunc(node->yoga, nullptr);
        if (wantsMeasure) YGNodeMarkDirty(node->yoga);
    }

    // -------------------------------------------------------------- layout

    void computeRects(Node* node, Vec2 origin) {
        const float left = YGNodeLayoutGetLeft(node->yoga);
        const float top = YGNodeLayoutGetTop(node->yoga);
        node->rect = Rect{{origin.x + left, origin.y + top},
                          {YGNodeLayoutGetWidth(node->yoga), YGNodeLayoutGetHeight(node->yoga)}};

        Vec2 childOrigin = node->rect.pos - node->scrollOffset;
        Vec2 extent{0.0f};
        for (auto& child : node->children) {
            computeRects(child.get(), childOrigin);
            extent = glm::max(extent, Vec2{YGNodeLayoutGetLeft(child->yoga) +
                                               YGNodeLayoutGetWidth(child->yoga),
                                           YGNodeLayoutGetTop(child->yoga) +
                                               YGNodeLayoutGetHeight(child->yoga)});
        }
        node->contentSize =
            extent + Vec2{YGNodeLayoutGetPadding(node->yoga, YGEdgeRight),
                          YGNodeLayoutGetPadding(node->yoga, YGEdgeBottom)};
    }

    void layout() {
        const VkExtent2D extent = app->swapchain->extent();
        const auto width = static_cast<float>(extent.width);
        const auto height = static_cast<float>(extent.height);
        YGNodeStyleSetWidth(root->yoga, width);
        YGNodeStyleSetHeight(root->yoga, height);
        YGNodeCalculateLayout(root->yoga, width, height, YGDirectionLTR);
        computeRects(root.get(), {0.0f, 0.0f});
    }

    // --------------------------------------------------------------- paint

    void paintNode(Node* node, Canvas& canvas, float opacity) {
        const css::ComputedStyle& s = node->style;
        if (s.display == Display::None) return;
        opacity *= s.opacity;
        if (opacity <= 0.0f) return;

        if (s.backgroundColor.a > 0.0f || (s.borderWidth > 0.0f && s.borderColor.a > 0.0f)) {
            canvas.drawRect(node->rect,
                            {.color = s.backgroundColor.fade(opacity),
                             .cornerRadii = s.borderRadius,
                             .borderWidth = s.borderWidth,
                             .borderColor = s.borderColor.fade(opacity)});
        }

        const bool clips = s.overflow != Overflow::Visible;
        if (clips) canvas.pushClip(node->rect);

        if (!node->text.empty() && node->children.empty()) {
            const FontRef font = resolveFont(s.fontFamily);
            const DrawTextOptions options{.font = font, .size = s.fontSize,
                                          .color = s.textColor.fade(opacity)};
            const float padLeft = YGNodeLayoutGetPadding(node->yoga, YGEdgeLeft);
            const float padTop = YGNodeLayoutGetPadding(node->yoga, YGEdgeTop);
            const float padRight = YGNodeLayoutGetPadding(node->yoga, YGEdgeRight);
            Vec2 pos = node->rect.pos + Vec2{padLeft, padTop};
            if (s.textAlign != TextAlign::Left) {
                const float avail = node->rect.size.x - padLeft - padRight;
                const float textWidth = canvas.measureText(node->text, options).x;
                if (s.textAlign == TextAlign::Center) pos.x += (avail - textWidth) * 0.5f;
                if (s.textAlign == TextAlign::Right) pos.x += avail - textWidth;
            }
            canvas.drawText(node->text, pos, options);
        }

        for (auto& child : node->children) paintNode(child.get(), canvas, opacity);

        if (clips) canvas.popClip();
    }

    // ---------------------------------------------------------- hot reload

    void checkHotReload() {
        const double now = static_cast<double>(SDL_GetTicksNS()) * 1e-9;
        if (now - lastWatchCheck < 0.5) return;
        lastWatchCheck = now;
        for (WatchedFile& file : watched) {
            std::error_code ec;
            const auto mtime = std::filesystem::last_write_time(file.path, ec);
            if (ec || mtime == file.mtime) continue;
            file.mtime = mtime;
            std::ifstream stream(file.path);
            std::stringstream buffer;
            buffer << stream.rdbuf();
            auto sheet = css::parse(buffer.str());
            if (sheet) {
                sheets[file.sheetIndex] = std::move(sheet).value();
                markDirty();
                log::info("css: reloaded {}", file.path);
            }
        }
    }
};

} // namespace detail

// ------------------------------------------------------------------ Element

using detail::Node;

Element Element::addChild(std::string_view tag, const ElementDesc& desc) {
    if (node_ == nullptr) return Element();
    Node* child = node_->ctx->newNode(tag);
    child->parent = node_;
    child->id = desc.id;
    child->text = desc.text;
    std::istringstream classes(desc.classes);
    std::string cls;
    while (classes >> cls) child->classes.push_back(cls);

    auto owned = node_->ctx->takeLoose(child);
    YGNodeInsertChild(node_->yoga, child->yoga,
                      static_cast<size_t>(YGNodeGetChildCount(node_->yoga)));
    node_->children.push_back(std::move(owned));
    node_->ctx->markDirty();
    return Element(child);
}

void Element::remove() {
    if (node_ == nullptr || node_->parent == nullptr) return;
    Node* parent = node_->parent;
    node_->ctx->forgetNode(node_);
    YGNodeRemoveChild(parent->yoga, node_->yoga);
    auto& siblings = parent->children;
    siblings.erase(std::remove_if(siblings.begin(), siblings.end(),
                                  [this](const auto& p) { return p.get() == node_; }),
                   siblings.end());
    parent->ctx->markDirty();
    node_ = nullptr;
}

void Element::clearChildren() {
    if (node_ == nullptr) return;
    for (auto& child : node_->children) node_->ctx->forgetNode(child.get());
    YGNodeRemoveAllChildren(node_->yoga);
    node_->children.clear();
    node_->ctx->markDirty();
}

Element& Element::setText(std::string text) {
    if (node_ != nullptr && node_->text != text) {
        node_->text = std::move(text);
        node_->ctx->markDirty();
    }
    return *this;
}

Element& Element::setClasses(std::string spaceSeparated) {
    if (node_ == nullptr) return *this;
    node_->classes.clear();
    std::istringstream stream(spaceSeparated);
    std::string cls;
    while (stream >> cls) node_->classes.push_back(cls);
    node_->ctx->markDirty();
    return *this;
}

Element& Element::addClass(const std::string& name) {
    if (node_ == nullptr) return *this;
    auto& classes = node_->classes;
    if (std::find(classes.begin(), classes.end(), name) == classes.end()) {
        classes.push_back(name);
        node_->ctx->markDirty();
    }
    return *this;
}

Element& Element::removeClass(const std::string& name) {
    if (node_ == nullptr) return *this;
    auto& classes = node_->classes;
    const auto it = std::find(classes.begin(), classes.end(), name);
    if (it != classes.end()) {
        classes.erase(it);
        node_->ctx->markDirty();
    }
    return *this;
}

Element& Element::setStyle(const Style& style) {
    if (node_ != nullptr) {
        node_->inlineStyle = style.declarations();
        node_->ctx->markDirty();
    }
    return *this;
}

Element& Element::onClick(std::function<void(Element)> handler) {
    if (node_ != nullptr) node_->onClick = std::move(handler);
    return *this;
}

Element& Element::setDisabled(bool disabled) {
    if (node_ != nullptr) node_->ctx->setPseudo(node_, css::kPseudoDisabled, disabled);
    return *this;
}

const std::string& Element::text() const {
    static const std::string empty;
    return node_ != nullptr ? node_->text : empty;
}

Rect Element::bounds() const { return node_ != nullptr ? node_->rect : Rect{}; }

bool Element::hovered() const {
    return node_ != nullptr && (node_->pseudo & css::kPseudoHover) != 0;
}

// ------------------------------------------------------------------ Context

Context::Context(App& app) : impl_(std::make_unique<detail::ContextImpl>()) {
    impl_->app = app.impl_.get();
    impl_->yogaConfig = YGConfigNew();
    YGConfigSetUseWebDefaults(impl_->yogaConfig, true); // stretch, shrink=1
    Node* rootNode = impl_->newNode("root");
    impl_->root = impl_->takeLoose(rootNode);
}

Context::Context(Context&&) noexcept = default;
Context& Context::operator=(Context&&) noexcept = default;

Context::~Context() {
    if (impl_ != nullptr && impl_->yogaConfig != nullptr) {
        impl_->root.reset();
        impl_->looseNodes.clear();
        YGConfigFree(impl_->yogaConfig);
    }
}

Result<void> Context::loadStylesheet(const std::string& path) {
    std::ifstream stream(path);
    if (!stream) return err("cannot open stylesheet '{}'", path);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    auto sheet = css::parse(buffer.str());
    if (!sheet) return sheet.error();
    impl_->sheets.push_back(std::move(sheet).value());
#ifndef NDEBUG
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (!ec) impl_->watched.push_back({path, impl_->sheets.size() - 1, mtime});
#endif
    impl_->markDirty();
    return {};
}

void Context::addStylesheet(std::string_view cssText) {
    auto sheet = css::parse(cssText);
    if (sheet) {
        impl_->sheets.push_back(std::move(sheet).value());
        impl_->markDirty();
    }
}

void Context::registerFont(const std::string& name, FontRef font) {
    impl_->fonts[name] = font;
    impl_->markDirty();
}

Element Context::root() { return Element(impl_->root.get()); }

void Context::update() {
    impl_->checkHotReload();
    impl_->processInput();
    if (impl_->dirty) {
        impl_->restyle();
        impl_->layout();
        impl_->dirty = false;
    }
}

void Context::paint(Canvas canvas) {
    // Viewport may have resized since the last layout.
    if (impl_->root->rect.size != canvas.size()) {
        impl_->restyle();
        impl_->layout();
    }
    impl_->paintNode(impl_->root.get(), canvas, 1.0f);
}

} // namespace rendy::ui
