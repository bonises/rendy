#pragma once

/// \file ui.hpp
/// Retained UI: an element tree styled by CSS (files or strings, with hot
/// reload in debug) and/or typed Style objects, laid out with flexbox, and
/// painted through the Canvas each frame.
///
///     rendy::ui::Context ui(app);
///     ui.loadStylesheet("assets/app.css");
///     auto row = ui.root().addChild("div", {.classes = "toolbar"});
///     row.addChild("button", {.text = "Open"}).onClick([](auto&) { ... });
///     while (app.pollEvents()) {
///         auto frame = app.beginFrame({});
///         ui.update();
///         ui.paint(frame.canvas());
///         frame.present();
///     }

#include "../app/app.hpp"
#include "../core/rect.hpp"
#include "../core/result.hpp"
#include "style.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rendy::ui {

namespace detail {
struct Node;
struct ContextImpl;
} // namespace detail

struct ElementDesc {
    std::string classes; ///< space-separated
    std::string id;
    std::string text;
};

/// Lightweight handle to a tree node. Cheap to copy; invalid after the
/// element is removed.
class Element {
public:
    Element() = default;

    Element addChild(std::string_view tag, const ElementDesc& desc = {});
    /// Remove this element (and subtree) from the tree.
    void remove();
    void clearChildren();

    Element& setText(std::string text);
    Element& setClasses(std::string spaceSeparated);
    Element& addClass(const std::string& name);
    Element& removeClass(const std::string& name);
    /// Inline typed style — wins over stylesheets like an HTML style="".
    Element& setStyle(const Style& style);
    Element& onClick(std::function<void(Element)> handler);
    Element& setDisabled(bool disabled);

    // ---- "input" elements (single-line text fields) ----------------------
    // Create with addChild("input", ...). Clicking focuses; typing edits
    // (UTF-8, selection with shift/ctrl, mouse drag). Style with CSS like
    // any element; give it a width (text never wraps).

    /// Fired after every text change the user makes.
    Element& onChange(std::function<void(Element)> handler);
    /// Fired on Enter.
    Element& onSubmit(std::function<void(Element)> handler);
    /// Faded hint shown while the field is empty.
    Element& setPlaceholder(std::string text);
    /// Give this element keyboard focus (useful for inputs).
    Element& focus();
    [[nodiscard]] bool focused() const;

    [[nodiscard]] const std::string& text() const;
    /// Absolute pixel bounds from the latest layout.
    [[nodiscard]] Rect bounds() const;
    [[nodiscard]] bool hovered() const;
    [[nodiscard]] bool valid() const { return node_ != nullptr; }

private:
    friend struct detail::ContextImpl;
    friend class Context;
    explicit Element(detail::Node* node) : node_(node) {}
    detail::Node* node_ = nullptr;
};

class Context {
public:
    explicit Context(App& app);
    Context(Context&&) noexcept;
    Context& operator=(Context&&) noexcept;
    ~Context();

    /// Load a .css file. In debug builds the file is watched and hot-reloads.
    Result<void> loadStylesheet(const std::string& path);
    /// Add CSS from a string (no hot reload).
    void addStylesheet(std::string_view cssText);

    /// Make a loaded font available to CSS `font-family: name`.
    void registerFont(const std::string& name, FontRef font);

    /// Typed-API equivalent of CSS `@keyframes name { ... }`: each pair is
    /// (offset 0..1, keyframe style). Elements reference it with
    /// `Style{}.animation("name", ...)` or CSS `animation: name ...`; typed
    /// keyframes win over CSS ones with the same name.
    void addKeyframes(std::string_view name, std::vector<std::pair<float, Style>> frames);

    [[nodiscard]] Element root();

    /// Process input, restyle and relayout as needed. Once per frame,
    /// after App::pollEvents().
    void update();
    /// Paint the tree. Call between beginFrame and present.
    void paint(Canvas canvas);

private:
    std::unique_ptr<detail::ContextImpl> impl_;
};

} // namespace rendy::ui
