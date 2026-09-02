#pragma once

// Single-line text editing logic for the <input> widget: UTF-8-aware cursor
// movement, selection, insert/erase. Pure functions on a small state struct
// (GPU-free, unit-testable). Positions are byte offsets at codepoint
// boundaries.

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace rendy::ui::edit {

inline bool isContinuation(char c) {
    return (static_cast<uint8_t>(c) & 0xC0) == 0x80;
}

/// Byte offset of the codepoint before `pos` (0 at the start).
inline size_t prevCp(std::string_view text, size_t pos) {
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && isContinuation(text[pos])) --pos;
    return pos;
}

/// Byte offset of the codepoint after `pos` (clamped to size()).
inline size_t nextCp(std::string_view text, size_t pos) {
    if (pos >= text.size()) return text.size();
    ++pos;
    while (pos < text.size() && isContinuation(text[pos])) ++pos;
    return pos;
}

inline bool isWordByte(char c) {
    const auto u = static_cast<uint8_t>(c);
    return u >= 0x80 || std::isalnum(u) != 0 || c == '_';
}

/// Ctrl+Left target: start of the previous word.
inline size_t prevWord(std::string_view text, size_t pos) {
    while (pos > 0 && !isWordByte(text[prevCp(text, pos)])) pos = prevCp(text, pos);
    while (pos > 0 && isWordByte(text[prevCp(text, pos)])) pos = prevCp(text, pos);
    return pos;
}

/// Ctrl+Right target: end of the next word.
inline size_t nextWord(std::string_view text, size_t pos) {
    while (pos < text.size() && !isWordByte(text[pos])) pos = nextCp(text, pos);
    while (pos < text.size() && isWordByte(text[pos])) pos = nextCp(text, pos);
    return pos;
}

/// Editable state: `cursor` is the caret, `anchor` the other end of the
/// selection (== cursor when nothing is selected).
struct State {
    std::string text;
    size_t cursor = 0;
    size_t anchor = 0;

    [[nodiscard]] bool hasSelection() const { return cursor != anchor; }
    [[nodiscard]] size_t selBegin() const { return std::min(cursor, anchor); }
    [[nodiscard]] size_t selEnd() const { return std::max(cursor, anchor); }
};

inline void eraseSelection(State* s) {
    if (!s->hasSelection()) return;
    s->text.erase(s->selBegin(), s->selEnd() - s->selBegin());
    s->cursor = s->anchor = s->selBegin();
}

/// Insert typed text at the caret (replacing any selection). Line breaks
/// are stripped — the field is single-line, and pasted/IME text can carry
/// \r or \n. Returns true when the text changed.
inline bool insert(State* s, std::string_view utf8) {
    std::string filtered;
    filtered.reserve(utf8.size());
    for (char c : utf8)
        if (c != '\n' && c != '\r') filtered += c;
    if (filtered.empty() && !s->hasSelection()) return false;
    eraseSelection(s);
    s->text.insert(s->cursor, filtered);
    s->cursor += filtered.size();
    s->anchor = s->cursor;
    return true;
}

/// Backspace. Returns true when the text changed.
inline bool eraseBackward(State* s, bool word = false) {
    if (s->hasSelection()) {
        eraseSelection(s);
        return true;
    }
    if (s->cursor == 0) return false;
    const size_t from = word ? prevWord(s->text, s->cursor) : prevCp(s->text, s->cursor);
    s->text.erase(from, s->cursor - from);
    s->cursor = s->anchor = from;
    return true;
}

/// Delete. Returns true when the text changed.
inline bool eraseForward(State* s, bool word = false) {
    if (s->hasSelection()) {
        eraseSelection(s);
        return true;
    }
    if (s->cursor >= s->text.size()) return false;
    const size_t to = word ? nextWord(s->text, s->cursor) : nextCp(s->text, s->cursor);
    s->text.erase(s->cursor, to - s->cursor);
    s->anchor = s->cursor;
    return true;
}

inline void moveTo(State* s, size_t pos, bool select) {
    s->cursor = std::min(pos, s->text.size());
    if (!select) s->anchor = s->cursor;
}

inline void moveLeft(State* s, bool select, bool word) {
    if (!select && s->hasSelection()) {
        // Collapse to the selection's left edge (browser behavior).
        s->cursor = s->anchor = s->selBegin();
        return;
    }
    moveTo(s, word ? prevWord(s->text, s->cursor) : prevCp(s->text, s->cursor), select);
}

inline void moveRight(State* s, bool select, bool word) {
    if (!select && s->hasSelection()) {
        s->cursor = s->anchor = s->selEnd();
        return;
    }
    moveTo(s, word ? nextWord(s->text, s->cursor) : nextCp(s->text, s->cursor), select);
}

inline void moveHome(State* s, bool select) { moveTo(s, 0, select); }
inline void moveEnd(State* s, bool select) { moveTo(s, s->text.size(), select); }

inline void selectAll(State* s) {
    s->anchor = 0;
    s->cursor = s->text.size();
}

} // namespace rendy::ui::edit
