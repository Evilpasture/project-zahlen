// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/gui/TextEdit.hpp
//
// The ECS binding of the text-editing rules: thin adapters that hand a
// UITextInputComponent's buffer and caret to the buffer-agnostic core in
// Zahlen/gui/TextBuffer.hpp.
//
// Engine::InitInternal's onKey/onChar callbacks forward here; nothing touches
// the window, the registry or the renderer, so the behaviour stays
// unit-testable without a display and the non-GLFW front end (the TTY backend)
// drives the very same code. The immediate-mode Context::TextInput calls the
// core directly with its own Caret, which is why the rules live over there and
// not in this file.
//
// Modifiers, ClipboardSink, KeyResult and Caret are declared in
// TextBuffer.hpp but live in this namespace, so they are reachable exactly
// where they always were.
//
// What the adapters add on top of the core is the two things only the component
// knows about:
//   * `edited` -- set when a call actually changed the text, so the legacy
//     immediate-mode builder knows to copy the string back to its owner.
//   * `isFocused` -- cleared on Enter/Escape. Focus is the component's field,
//     not the core's business.
#pragma once

#include <Zahlen/gui/TextBuffer.hpp>
#include <Zahlen/gui/UIComponents.hpp>
#include <string_view>

namespace ZHLN::GUI::TextEdit {

namespace detail {

// Re-exported so the word-boundary helpers keep one definition. ReplaceRange
// and MoveCaret are not re-exported: they take a buffer and a Caret, and an
// adapter that only had a component would just be forwarding.
using TextEdit::detail::IsWordChar;
using TextEdit::detail::NextWordBoundary;
using TextEdit::detail::PrevWordBoundary;

} // namespace detail

/// The selected text (empty when there is no selection).
[[nodiscard]] inline auto SelectedText(const UIComponents::UITextInputComponent& in) -> std::string_view {
    return SelectedText(in.text, in.caret);
}

/// Replaces the current selection (or inserts at the caret) with `text`.
/// Control characters and anything outside printable ASCII are dropped: the
/// atlas cannot draw them and a stray '\n' would break the single-line model.
inline auto InsertText(UIComponents::UITextInputComponent& in, std::string_view text) -> bool {
    const bool changed = InsertText(in.text, in.caret, text);
    in.edited          = in.edited || changed;
    return changed;
}

/// Deletes the selection if there is one, otherwise one character before the
/// caret (Backspace) or after it (Delete). Ctrl deletes to the word boundary.
inline auto DeleteAtCaret(UIComponents::UITextInputComponent& in, bool backward, bool wholeWord) -> bool {
    const bool changed = DeleteAtCaret(in.text, in.caret, backward, wholeWord);
    in.edited          = in.edited || changed;
    return changed;
}

/// A printable character typed into the field (the onChar path). Anything
/// outside 32..126 is ignored, matching what the font atlas can draw.
inline auto HandleChar(UIComponents::UITextInputComponent& in, unsigned int codepoint) -> bool {
    const bool changed = HandleChar(in.text, in.caret, codepoint);
    in.edited          = in.edited || changed;
    return changed;
}

/// A key press (or repeat) delivered to the focused field. `clipboard` may be
/// left empty, in which case Ctrl+C/X/V do nothing.
inline auto HandleKey(
    UIComponents::UITextInputComponent& in, KeyCode key, Modifiers mods, const ClipboardSink& clipboard = {}
) -> KeyResult {
    const KeyResult result = HandleKey(in.text, in.caret, key, mods, clipboard);
    if (result == KeyResult::Edited) {
        in.edited = true;
    }
    if (result == KeyResult::Committed) {
        in.isFocused = false;
    }
    return result;
}

} // namespace ZHLN::GUI::TextEdit
