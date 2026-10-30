// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Input.hpp>
#include <algorithm>
#include <cctype>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace ZHLN::GUI::TextEdit {

struct Modifiers {
    bool shift = false;
    bool ctrl  = false;
};

struct ClipboardSink {
    void*       userdata                                       = nullptr;
    void        (*set)(void* userdata, std::string_view text)  = nullptr;
    std::string (*get)(void* userdata)                         = nullptr;
};

enum class KeyResult : uint8_t {
    Ignored,
    Navigated,
    Edited,
    Committed
};

struct Caret {
    uint32_t cursorIndex     = 0;
    uint32_t selectionAnchor = 0;
    bool selectAll = false;

    [[nodiscard]] constexpr auto HasSelection() const noexcept -> bool {
        return selectAll || selectionAnchor != cursorIndex;
    }

    [[nodiscard]] constexpr auto SelectionStart() const noexcept -> uint32_t {
        return selectAll ? 0u : std::min(selectionAnchor, cursorIndex);
    }

    [[nodiscard]] constexpr auto SelectionEnd(size_t textLength) const noexcept -> uint32_t {
        const auto len = static_cast<uint32_t>(textLength);
        return selectAll ? len : std::min(std::max(selectionAnchor, cursorIndex), len);
    }

    constexpr void ClearSelection() noexcept {
        selectAll       = false;
        selectionAnchor = cursorIndex;
    }
};

template <typename B>
concept TextBuffer = requires(const B& cb, B& b, std::string_view sv) {
    { std::string_view(cb) } -> std::convertible_to<std::string_view>;
    { cb.size() } -> std::convertible_to<size_t>;
    b.assign(sv);
};

template <typename B>
[[nodiscard]] constexpr auto MaxTextLength(const B& buf) noexcept -> size_t {
    if constexpr (requires { buf.maxTextLength(); }) {
        return buf.maxTextLength();
    } else if constexpr (requires { B::kMaxTextLength; }) {
        return B::kMaxTextLength;
    } else {
        return std::numeric_limits<size_t>::max();
    }
}

struct BoundedString {
    std::string* text      = nullptr;
    size_t       maxLength = std::numeric_limits<size_t>::max();

    [[nodiscard]] operator std::string_view() const noexcept {
        return *text;
    }
    [[nodiscard]] auto size() const noexcept -> size_t {
        return text->size();
    }
    [[nodiscard]] auto maxTextLength() const noexcept -> size_t {
        return maxLength;
    }
    void assign(std::string_view sv) {
        text->assign(sv);
    }
};

namespace TemplatedDetail {

[[nodiscard]] inline auto IsWordChar(char c) noexcept -> bool {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '_';
}

[[nodiscard]] inline auto PrevWordBoundary(std::string_view text, size_t pos) noexcept -> size_t {
    pos = std::min(pos, text.size());
    while (pos > 0 && !IsWordChar(text[pos - 1])) {
        --pos;
    }
    while (pos > 0 && IsWordChar(text[pos - 1])) {
        --pos;
    }
    return pos;
}

[[nodiscard]] inline auto NextWordBoundary(std::string_view text, size_t pos) noexcept -> size_t {
    pos = std::min(pos, text.size());
    while (pos < text.size() && !IsWordChar(text[pos])) {
        ++pos;
    }
    while (pos < text.size() && IsWordChar(text[pos])) {
        ++pos;
    }
    return pos;
}

template <TextBuffer B>
inline auto ReplaceRange(B& buf, Caret& caret, size_t start, size_t end, std::string_view replacement) -> bool {
    const std::string_view curr = buf;
    start                       = std::min(start, curr.size());
    end                         = std::clamp(end, start, curr.size());
    if (start == end && replacement.empty()) {
        return false;
    }

    const size_t kept     = curr.size() - (end - start);
    const size_t room     = kept < MaxTextLength(buf) ? MaxTextLength(buf) - kept : 0;
    const size_t accepted = std::min(replacement.size(), room);

    std::string next;
    next.reserve(kept + accepted);
    next.append(curr.substr(0, start));
    next.append(replacement.substr(0, accepted));
    next.append(curr.substr(end));

    buf.assign(next);

    caret.cursorIndex     = static_cast<uint32_t>(start + accepted);
    caret.selectionAnchor = caret.cursorIndex;
    caret.selectAll       = false;
    return true;
}

inline void MoveCaret(std::string_view text, Caret& caret, size_t target, bool extendSelection) {
    const auto len = static_cast<uint32_t>(text.size());
    if (caret.selectAll) {
        caret.selectionAnchor = 0;
        caret.cursorIndex     = len;
        caret.selectAll       = false;
    }
    caret.cursorIndex = static_cast<uint32_t>(std::min(target, static_cast<size_t>(len)));
    if (!extendSelection) {
        caret.selectionAnchor = caret.cursorIndex;
    }
}

}

template <TextBuffer B>
[[nodiscard]] inline auto SelectedText(const B& buf, const Caret& caret) -> std::string_view {
    if (!caret.HasSelection()) {
        return {};
    }
    const std::string_view curr = buf;
    const uint32_t         s    = caret.SelectionStart();
    const uint32_t         e    = caret.SelectionEnd(curr.size());
    return curr.substr(s, e - s);
}

template <TextBuffer B>
inline auto InsertText(B& buf, Caret& caret, std::string_view text) -> bool {
    std::string clean;
    clean.reserve(text.size());
    for (char c: text) {
        if (c >= 32 && c <= 126) {
            clean.push_back(c);
        }
    }
    const size_t start = caret.HasSelection() ? caret.SelectionStart() : caret.cursorIndex;
    const size_t end   = caret.HasSelection() ? caret.SelectionEnd(buf.size()) : caret.cursorIndex;
    return TemplatedDetail::ReplaceRange(buf, caret, start, end, clean);
}

template <TextBuffer B>
inline auto DeleteAtCaret(B& buf, Caret& caret, bool backward, bool wholeWord) -> bool {
    if (caret.HasSelection()) {
        return TemplatedDetail::ReplaceRange(buf, caret, caret.SelectionStart(), caret.SelectionEnd(buf.size()), {});
    }
    const std::string_view curr  = buf;
    const size_t           caretPos = std::min<size_t>(caret.cursorIndex, curr.size());
    if (backward) {
        if (caretPos == 0) {
            return false;
        }
        const size_t start = wholeWord ? TemplatedDetail::PrevWordBoundary(curr, caretPos) : caretPos - 1;
        return TemplatedDetail::ReplaceRange(buf, caret, start, caretPos, {});
    }
    if (caretPos >= curr.size()) {
        return false;
    }
    const size_t end = wholeWord ? TemplatedDetail::NextWordBoundary(curr, caretPos) : caretPos + 1;
    return TemplatedDetail::ReplaceRange(buf, caret, caretPos, end, {});
}

template <TextBuffer B>
inline auto HandleChar(B& buf, Caret& caret, unsigned int codepoint) -> bool {
    if (codepoint < 32 || codepoint > 126) {
        return false;
    }
    const char c = static_cast<char>(codepoint);
    return InsertText(buf, caret, std::string_view(&c, 1));
}

template <TextBuffer B>
inline auto HandleKey(B& buf, Caret& caret, KeyCode key, Modifiers mods, const ClipboardSink& clipboard = {}) -> KeyResult {
    const std::string_view curr     = buf;
    const size_t           caretPos = std::min<size_t>(caret.cursorIndex, curr.size());

    switch (key) {
        case KeyCode::Backspace:
            return DeleteAtCaret(buf, caret, true, mods.ctrl) ? KeyResult::Edited : KeyResult::Navigated;
        case KeyCode::Delete:
            return DeleteAtCaret(buf, caret, false, mods.ctrl) ? KeyResult::Edited : KeyResult::Navigated;

        case KeyCode::Left:
            if (caret.HasSelection() && !mods.shift && !mods.ctrl) {
                TemplatedDetail::MoveCaret(curr, caret, caret.SelectionStart(), false);
            } else {
                const size_t target = mods.ctrl ? TemplatedDetail::PrevWordBoundary(curr, caretPos) : (caretPos > 0 ? caretPos - 1 : 0);
                TemplatedDetail::MoveCaret(curr, caret, target, mods.shift);
            }
            return KeyResult::Navigated;
        case KeyCode::Right:
            if (caret.HasSelection() && !mods.shift && !mods.ctrl) {
                TemplatedDetail::MoveCaret(curr, caret, caret.SelectionEnd(curr.size()), false);
            } else {
                const size_t target = mods.ctrl ? TemplatedDetail::NextWordBoundary(curr, caretPos) : std::min(caretPos + 1, curr.size());
                TemplatedDetail::MoveCaret(curr, caret, target, mods.shift);
            }
            return KeyResult::Navigated;
        case KeyCode::Home:
            TemplatedDetail::MoveCaret(curr, caret, 0, mods.shift);
            return KeyResult::Navigated;
        case KeyCode::End:
            TemplatedDetail::MoveCaret(curr, caret, curr.size(), mods.shift);
            return KeyResult::Navigated;

        case KeyCode::A:
            if (mods.ctrl) {
                caret.selectAll       = true;
                caret.selectionAnchor = 0;
                caret.cursorIndex     = static_cast<uint32_t>(curr.size());
                return KeyResult::Navigated;
            }
            return KeyResult::Ignored;
        case KeyCode::C:
            if (mods.ctrl) {
                if (clipboard.set != nullptr && caret.HasSelection()) {
                    clipboard.set(clipboard.userdata, SelectedText(buf, caret));
                }
                return KeyResult::Navigated;
            }
            return KeyResult::Ignored;
        case KeyCode::X:
            if (mods.ctrl) {
                if (!caret.HasSelection()) {
                    return KeyResult::Navigated;
                }
                if (clipboard.set != nullptr) {
                    clipboard.set(clipboard.userdata, SelectedText(buf, caret));
                }
                return DeleteAtCaret(buf, caret, true, false) ? KeyResult::Edited : KeyResult::Navigated;
            }
            return KeyResult::Ignored;
        case KeyCode::V:
            if (mods.ctrl) {
                if (clipboard.get == nullptr) {
                    return KeyResult::Navigated;
                }
                const std::string pasted = clipboard.get(clipboard.userdata);
                return InsertText(buf, caret, pasted) ? KeyResult::Edited : KeyResult::Navigated;
            }
            return KeyResult::Ignored;

        case KeyCode::Enter:
        case KeyCode::Escape:
            caret.ClearSelection();
            return KeyResult::Committed;

        default:
            return KeyResult::Ignored;
    }
}

}
