// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/gui/TextEdit.hpp
//
// Keyboard editing for UITextInputComponent: caret movement, selection,
// insertion, deletion and clipboard exchange, as one pure function over the
// component and a key event. Engine::InitInternal's onKey/onChar callbacks
// forward to these; nothing here touches the window, the registry or the
// renderer, so the behaviour is unit-testable without a display and a
// non-GLFW front end (the TTY backend) drives the very same code.
//
// The single-line model:
//   * The caret is `cursorIndex`, a byte offset into `text` (ASCII only: the
//     font atlas covers 32..126, and onChar rejects everything else).
//   * The selection is [SelectionStart, SelectionEnd) from `selectionAnchor`
//     and `cursorIndex`, or the whole text while `selectAll` is set. Any
//     un-shifted caret move collapses it; any edit replaces it.
//   * Every mutation sets `edited`, which the immediate-mode builder reads to
//     copy the text back into the caller's string.
//
// Clipboard access goes through the small ClipboardSink interface so the key
// handler does not depend on Window: the engine wires it to Window's clipboard,
// tests wire it to a std::string.
#pragma once

#include <Zahlen/Input.hpp>
#include <Zahlen/gui/UIComponents.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace ZHLN::GUI::TextEdit {

/// Modifier state captured at the moment of the key press.
struct Modifiers {
    bool shift = false;
    bool ctrl  = false; // Also Cmd on macOS front ends that map it here
};

/// Where Ctrl+C / Ctrl+X write and Ctrl+V reads. Implementations may be
/// stateless views over the OS clipboard or a plain in-memory buffer.
struct ClipboardSink {
    void*       userdata                                                = nullptr;
    void        (*set)(void* userdata, std::string_view text)          = nullptr;
    std::string (*get)(void* userdata)                                  = nullptr;
};

/// Outcome of HandleKey, so the caller can decide whether the event was
/// consumed by the text field (and must not fall through to gameplay/hotkeys).
enum class KeyResult : uint8_t {
    Ignored,   // Not a text-editing key; nothing changed
    Navigated, // Caret/selection changed, text did not
    Edited,    // Text changed (`edited` is set)
    Committed  // Enter/Escape: focus released
};

namespace detail {

[[nodiscard]] inline auto IsWordChar(char c) noexcept -> bool {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '_';
}

/// Start of the word to the left of `pos` (Ctrl+Left), the way editors move:
/// skip separators first, then the run of word characters.
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

/// End of the word to the right of `pos` (Ctrl+Right).
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

/// Replaces [start, end) with `replacement`, clamps to String256's capacity
/// and parks the caret after the inserted text. Returns false when nothing
/// could change (empty replacement over an empty range).
inline auto ReplaceRange(UIComponents::UITextInputComponent& in, size_t start, size_t end, std::string_view replacement) -> bool {
    std::string_view curr = in.text;
    start                 = std::min(start, curr.size());
    end                   = std::clamp(end, start, curr.size());
    if (start == end && replacement.empty()) {
        return false;
    }

    // FixedString has no insert/erase; rebuild through a scratch string. The
    // capacity minus terminator bounds how much of the replacement survives.
    const size_t room     = in.text.capacity() - 1 - (curr.size() - (end - start));
    const size_t accepted = std::min(replacement.size(), room);

    std::string next;
    next.reserve(curr.size() - (end - start) + accepted);
    next.append(curr.substr(0, start));
    next.append(replacement.substr(0, accepted));
    next.append(curr.substr(end));

    in.text.assign(next);
    in.cursorIndex     = static_cast<uint32_t>(start + accepted);
    in.selectionAnchor = in.cursorIndex;
    in.selectAll       = false;
    in.edited          = true;
    return true;
}

/// Moves the caret to `target`; with Shift the anchor stays (extending or
/// starting a selection), without it the selection collapses onto the caret.
inline void MoveCaret(UIComponents::UITextInputComponent& in, size_t target, bool extendSelection) {
    const auto len = static_cast<uint32_t>(in.text.size());
    // A whole-text selection is anchored at 0 with the caret at the end;
    // materialise that before shift-extending so the anchor is meaningful.
    if (in.selectAll) {
        in.selectionAnchor = 0;
        in.cursorIndex     = len;
        in.selectAll       = false;
    }
    in.cursorIndex = static_cast<uint32_t>(std::min(target, static_cast<size_t>(len)));
    if (!extendSelection) {
        in.selectionAnchor = in.cursorIndex;
    }
}

} // namespace detail

/// The selected text (empty when there is no selection).
[[nodiscard]] inline auto SelectedText(const UIComponents::UITextInputComponent& in) -> std::string_view {
    if (!in.HasSelection()) {
        return {};
    }
    const std::string_view curr = in.text;
    const uint32_t         s    = in.SelectionStart();
    const uint32_t         e    = in.SelectionEnd();
    return curr.substr(s, e - s);
}

/// Replaces the current selection (or inserts at the caret) with `text`.
/// Control characters and anything outside printable ASCII are dropped: the
/// atlas cannot draw them and a stray '\n' would break the single-line model.
inline auto InsertText(UIComponents::UITextInputComponent& in, std::string_view text) -> bool {
    std::string clean;
    clean.reserve(text.size());
    for (char c: text) {
        if (c >= 32 && c <= 126) {
            clean.push_back(c);
        }
    }
    const size_t start = in.HasSelection() ? in.SelectionStart() : in.cursorIndex;
    const size_t end   = in.HasSelection() ? in.SelectionEnd() : in.cursorIndex;
    return detail::ReplaceRange(in, start, end, clean);
}

/// Deletes the selection if there is one, otherwise one character before the
/// caret (Backspace) or after it (Delete). Ctrl deletes to the word boundary.
inline auto DeleteAtCaret(UIComponents::UITextInputComponent& in, bool backward, bool wholeWord) -> bool {
    if (in.HasSelection()) {
        return detail::ReplaceRange(in, in.SelectionStart(), in.SelectionEnd(), {});
    }
    const std::string_view curr  = in.text;
    const size_t           caret = std::min<size_t>(in.cursorIndex, curr.size());
    if (backward) {
        if (caret == 0) {
            return false;
        }
        const size_t start = wholeWord ? detail::PrevWordBoundary(curr, caret) : caret - 1;
        return detail::ReplaceRange(in, start, caret, {});
    }
    if (caret >= curr.size()) {
        return false;
    }
    const size_t end = wholeWord ? detail::NextWordBoundary(curr, caret) : caret + 1;
    return detail::ReplaceRange(in, caret, end, {});
}

/// A printable character typed into the field (the onChar path). Anything
/// outside 32..126 is ignored, matching what the font atlas can draw.
inline auto HandleChar(UIComponents::UITextInputComponent& in, unsigned int codepoint) -> bool {
    if (codepoint < 32 || codepoint > 126) {
        return false;
    }
    const char c = static_cast<char>(codepoint);
    return InsertText(in, std::string_view(&c, 1));
}

/// A key press (or repeat) delivered to the focused field. `clipboard` may be
/// left empty, in which case Ctrl+C/X/V do nothing.
inline auto HandleKey(
    UIComponents::UITextInputComponent& in, KeyCode key, Modifiers mods, const ClipboardSink& clipboard = {}
) -> KeyResult {
    const std::string_view curr  = in.text;
    const size_t           caret = std::min<size_t>(in.cursorIndex, curr.size());

    switch (key) {
        case KeyCode::Backspace:
            return DeleteAtCaret(in, true, mods.ctrl) ? KeyResult::Edited : KeyResult::Navigated;
        case KeyCode::Delete:
            return DeleteAtCaret(in, false, mods.ctrl) ? KeyResult::Edited : KeyResult::Navigated;

        case KeyCode::Left:
            if (in.HasSelection() && !mods.shift && !mods.ctrl) {
                // Collapsing a selection lands the caret at its left edge,
                // not one step left of wherever the caret happened to be.
                const size_t s = in.SelectionStart();
                detail::MoveCaret(in, s, false);
            } else {
                const size_t target = mods.ctrl ? detail::PrevWordBoundary(curr, caret) : (caret > 0 ? caret - 1 : 0);
                detail::MoveCaret(in, target, mods.shift);
            }
            return KeyResult::Navigated;
        case KeyCode::Right:
            if (in.HasSelection() && !mods.shift && !mods.ctrl) {
                const size_t e = in.SelectionEnd();
                detail::MoveCaret(in, e, false);
            } else {
                const size_t target = mods.ctrl ? detail::NextWordBoundary(curr, caret) : std::min(caret + 1, curr.size());
                detail::MoveCaret(in, target, mods.shift);
            }
            return KeyResult::Navigated;
        case KeyCode::Home:
            detail::MoveCaret(in, 0, mods.shift);
            return KeyResult::Navigated;
        case KeyCode::End:
            detail::MoveCaret(in, curr.size(), mods.shift);
            return KeyResult::Navigated;

        case KeyCode::A:
            if (mods.ctrl) {
                in.selectAll       = true;
                in.selectionAnchor = 0;
                in.cursorIndex     = static_cast<uint32_t>(curr.size());
                return KeyResult::Navigated;
            }
            return KeyResult::Ignored;
        case KeyCode::C:
            if (mods.ctrl) {
                if (clipboard.set != nullptr && in.HasSelection()) {
                    clipboard.set(clipboard.userdata, SelectedText(in));
                }
                return KeyResult::Navigated;
            }
            return KeyResult::Ignored;
        case KeyCode::X:
            if (mods.ctrl) {
                if (!in.HasSelection()) {
                    return KeyResult::Navigated;
                }
                if (clipboard.set != nullptr) {
                    clipboard.set(clipboard.userdata, SelectedText(in));
                }
                return DeleteAtCaret(in, true, false) ? KeyResult::Edited : KeyResult::Navigated;
            }
            return KeyResult::Ignored;
        case KeyCode::V:
            if (mods.ctrl) {
                if (clipboard.get == nullptr) {
                    return KeyResult::Navigated;
                }
                const std::string pasted = clipboard.get(clipboard.userdata);
                return InsertText(in, pasted) ? KeyResult::Edited : KeyResult::Navigated;
            }
            return KeyResult::Ignored;

        case KeyCode::Enter:
        case KeyCode::Escape:
            // Commit/defocus: leave focus but keep the text.
            in.isFocused = false;
            in.ClearSelection();
            return KeyResult::Committed;

        default:
            return KeyResult::Ignored;
    }
}

} // namespace ZHLN::GUI::TextEdit
