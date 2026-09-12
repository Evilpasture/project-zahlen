// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/gui/TextBuffer.hpp
//
// The buffer-agnostic half of text editing: caret movement, selection,
// insertion, deletion and clipboard exchange, expressed over any store that
// can be read as a std::string_view and reassigned from one.
//
// Nothing here knows about the registry, the window or the renderer, so any
// front end can drive it. Today that is ZHLN::GUI::Context::TextInput, which
// keeps a Caret per widget in its own state table, and tests/core/TestTextEdit.cpp
// over a plain std::string.
//
// The rules used to be free functions taking a UITextInputComponent&, the ECS
// text field. That component is gone: nothing set its isFocused, so the engine's
// forwarding loops could never fire, and nothing read its `edited` flag or drew
// it. Immediate mode keeps per-field state beside the widget, which is where a
// Caret belongs -- so the component had no reason to exist once the rules moved
// behind this concept.
//
// The single-line model:
//   * The caret is Caret::cursorIndex, a byte offset into the buffer (ASCII
//     only: the font atlas covers 32..126, and HandleChar rejects the rest).
//   * The selection is [SelectionStart(), SelectionEnd(len)) from
//     `selectionAnchor` and `cursorIndex`, or the whole buffer while
//     `selectAll` is set. Any un-shifted caret move collapses it; any edit
//     replaces it.
//
// Clipboard access goes through the small ClipboardSink interface so nothing
// here depends on Window: the engine wires it to Window's clipboard, tests wire
// it to a std::string.
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

/// Modifier state captured at the moment of the key press.
struct Modifiers {
    bool shift = false;
    bool ctrl  = false; // Also Cmd on macOS front ends that map it here
};

/// Where Ctrl+C / Ctrl+X write and Ctrl+V reads. Implementations may be
/// stateless views over the OS clipboard or a plain in-memory buffer.
struct ClipboardSink {
    void*       userdata                                       = nullptr;
    void        (*set)(void* userdata, std::string_view text)  = nullptr;
    std::string (*get)(void* userdata)                         = nullptr;
};

/// Outcome of HandleKey, so the caller can decide whether the event was
/// consumed by the text field (and must not fall through to gameplay/hotkeys).
enum class KeyResult : uint8_t {
    Ignored,   // Not a text-editing key; nothing changed
    Navigated, // Caret/selection changed, text did not
    Edited,    // Text changed
    Committed  // Enter/Escape: the caller should release focus
};

/// Caret and selection state, kept apart from the bytes it indexes so it can
/// live wherever a front end keeps per-widget state: inline in an ECS
/// component, or in the immediate-mode Context's widget table.
struct Caret {
    uint32_t cursorIndex     = 0;
    uint32_t selectionAnchor = 0;
    // Set on the unfocused->focused transition: the pre-focus content is
    // "selected", so the first printable key REPLACES it ("Default" goes away
    // when you type) and Backspace deletes it wholesale. Any caret movement
    // (Left/Right) or commit clears it, matching how name fields behave in tool
    // UIs. Equivalent to a selection spanning the whole text; kept as a flag so
    // the focus-gain path does not need the text length and so a renderer can
    // highlight without measuring.
    bool selectAll = false;

    [[nodiscard]] constexpr auto HasSelection() const noexcept -> bool {
        return selectAll || selectionAnchor != cursorIndex;
    }

    /// `textLength` is the buffer's current length, because a whole-text
    /// selection ends at the end of the text rather than at a stored offset.
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

/// A text store the editor can work against.
///
/// Deliberately only two requirements. In particular there is no capacity():
/// FixedString::capacity() counts the terminator while std::string::capacity()
/// is just the current allocation, so neither means "how much text fits". See
/// MaxTextLength below for how the bound is actually obtained.
template <typename B>
concept TextBuffer = requires(const B& cb, B& b, std::string_view sv) {
    { std::string_view(cb) } -> std::convertible_to<std::string_view>;
    { cb.size() } -> std::convertible_to<size_t>;
    b.assign(sv);
};

/// Longest text `buf` can hold.
///
/// A store that knows its own limit advertises it, either at run time through
/// `maxTextLength()` (BoundedString) or at compile time as `kMaxTextLength`
/// (FixedString: capacity minus the terminator). Anything unbounded, such as
/// std::string, grows on assign, so nothing is truncated here and the buffer
/// itself decides.
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

/// A std::string under a hard text limit.
///
/// Front ends whose real storage is fixed-capacity but which want to edit
/// through a std::string scratch wrap it in this, so a paste that will not fit
/// is shortened by the editor rather than being handed whole to a clamping
/// assign -- which would eat the tail of the buffer instead of the paste.
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

/// Replaces [start, end) with `replacement` and parks the caret after the
/// inserted text. Returns false when nothing could change (empty replacement
/// over an empty range).
///
/// How much of `replacement` survives is the store's business: FixedString
/// clamps on assign, std::string grows. So the replacement is cut to the room
/// that is actually left rather than handing an over-long string to assign and
/// letting the tail of the buffer be the casualty -- a paste into a nearly full
/// String64 must shorten the paste, not lose the text after the caret.
template <TextBuffer B>
inline auto ReplaceRange(B& buf, Caret& caret, size_t start, size_t end, std::string_view replacement) -> bool {
    const std::string_view curr = buf;
    start                       = std::min(start, curr.size());
    end                         = std::clamp(end, start, curr.size());
    if (start == end && replacement.empty()) {
        return false;
    }

    const size_t kept     = curr.size() - (end - start); // What survives either side of the range
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

/// Moves the caret to `target`; with Shift the anchor stays (extending or
/// starting a selection), without it the selection collapses onto the caret.
inline void MoveCaret(std::string_view text, Caret& caret, size_t target, bool extendSelection) {
    const auto len = static_cast<uint32_t>(text.size());
    // A whole-text selection is anchored at 0 with the caret at the end;
    // materialise that before shift-extending so the anchor is meaningful.
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

} // namespace TemplatedDetail

/// The selected text (empty when there is no selection).
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

/// Replaces the current selection (or inserts at the caret) with `text`.
/// Control characters and anything outside printable ASCII are dropped: the
/// atlas cannot draw them and a stray '\n' would break the single-line model.
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

/// Deletes the selection if there is one, otherwise one character before the
/// caret (Backspace) or after it (Delete). Ctrl deletes to the word boundary.
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

/// A printable character typed into the field (the onChar path). Anything
/// outside 32..126 is ignored, matching what the font atlas can draw.
template <TextBuffer B>
inline auto HandleChar(B& buf, Caret& caret, unsigned int codepoint) -> bool {
    if (codepoint < 32 || codepoint > 126) {
        return false;
    }
    const char c = static_cast<char>(codepoint);
    return InsertText(buf, caret, std::string_view(&c, 1));
}

/// A key press (or repeat) delivered to the focused field. `clipboard` may be
/// left empty, in which case Ctrl+C/X/V do nothing.
///
/// Committed means Enter/Escape: the selection is cleared here, but focus is
/// the caller's to release, because where focus lives differs per front end.
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
                // Collapsing a selection lands the caret at its left edge,
                // not one step left of wherever the caret happened to be.
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
            // Commit/defocus: keep the text, drop the selection.
            caret.ClearSelection();
            return KeyResult::Committed;

        default:
            return KeyResult::Ignored;
    }
}

} // namespace ZHLN::GUI::TextEdit
