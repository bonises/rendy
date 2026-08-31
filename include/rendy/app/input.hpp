#pragma once

/// \file input.hpp
/// Per-frame input state, refreshed by App::pollEvents(). "Pressed" and
/// "released" are edges for this frame; "down" is the held state.

#include "../math/math.hpp"

#include <bitset>
#include <cstdint>
#include <string>

namespace rendy {

namespace detail {
struct AppImpl;
}

enum class MouseButton : uint8_t { Left, Right, Middle, Count };

enum class Key : uint8_t {
    Unknown = 0,
    // clang-format off
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    // clang-format on
    Escape, Enter, Tab, Backspace, Space,
    Insert, Delete, Home, End, PageUp, PageDown,
    Left, Right, Up, Down,
    LeftShift, RightShift, LeftCtrl, RightCtrl, LeftAlt, RightAlt, Super,
    Minus, Equals, LeftBracket, RightBracket, Backslash,
    Semicolon, Apostrophe, Grave, Comma, Period, Slash,
    Count,
};

class Input {
public:
    [[nodiscard]] Vec2 mousePos() const { return mousePos_; }
    [[nodiscard]] Vec2 mouseDelta() const { return mouseDelta_; }
    /// Scroll this frame; +y is up/away, in "lines".
    [[nodiscard]] Vec2 wheel() const { return wheel_; }

    [[nodiscard]] bool mouseDown(MouseButton b) const { return mouseDown_[idx(b)]; }
    [[nodiscard]] bool mousePressed(MouseButton b) const { return mousePressed_[idx(b)]; }
    [[nodiscard]] bool mouseReleased(MouseButton b) const { return mouseReleased_[idx(b)]; }

    [[nodiscard]] bool keyDown(Key k) const { return keyDown_[idx(k)]; }
    /// True on initial press and on OS key-repeat (for editors).
    [[nodiscard]] bool keyPressed(Key k) const { return keyPressed_[idx(k)]; }
    [[nodiscard]] bool keyReleased(Key k) const { return keyReleased_[idx(k)]; }

    [[nodiscard]] bool ctrl() const { return keyDown(Key::LeftCtrl) || keyDown(Key::RightCtrl); }
    [[nodiscard]] bool shift() const {
        return keyDown(Key::LeftShift) || keyDown(Key::RightShift);
    }
    [[nodiscard]] bool alt() const { return keyDown(Key::LeftAlt) || keyDown(Key::RightAlt); }

    /// UTF-8 text typed this frame (respects layout, dead keys, IME).
    [[nodiscard]] const std::string& text() const { return text_; }

private:
    friend struct detail::AppImpl;
    static size_t idx(MouseButton b) { return static_cast<size_t>(b); }
    static size_t idx(Key k) { return static_cast<size_t>(k); }

    Vec2 mousePos_{0.0f};
    Vec2 mouseDelta_{0.0f};
    Vec2 wheel_{0.0f};
    std::bitset<static_cast<size_t>(MouseButton::Count)> mouseDown_, mousePressed_,
        mouseReleased_;
    std::bitset<static_cast<size_t>(Key::Count)> keyDown_, keyPressed_, keyReleased_;
    std::string text_;
};

} // namespace rendy
