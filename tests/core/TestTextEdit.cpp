// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// The editing rules behind ZHLN::GUI::Context::TextInput, exercised without a
// display: caret movement, selection, insertion, deletion, word boundaries and
// clipboard exchange over a plain std::string, a bounded scratch standing in for
// a fixed-capacity field, and a real FixedString.
//
// These used to drive a UITextInputComponent. That component is gone -- nothing
// ever set its isFocused, so the engine's forwarding loops could not fire -- and
// the rules it wrapped now live in Zahlen/gui/TextBuffer.hpp over any buffer.

#include "TestsFramework.hpp"
#include <Zahlen/Core/String.hpp>
#include <Zahlen/gui/TextBuffer.hpp>
#include <expected>
#include <string>
#include <string_view>

enum class TextEditTestError : uint8_t {
    NavigationFailed ZHLN_ANNOTATION(ZHLN::Description<"Caret navigation behaved incorrectly."> {}) = 1,
    EditFailed       ZHLN_ANNOTATION(ZHLN::Description<"Text insertion or deletion behaved incorrectly."> {}),
    SelectionFailed  ZHLN_ANNOTATION(ZHLN::Description<"Text selection logic behaved incorrectly."> {}),
};

namespace {

using ZHLN::GUI::TextEdit::Caret;
using ZHLN::GUI::TextEdit::Modifiers;

/// Caret parked at `pos` with no selection. Setting cursorIndex alone would
/// leave the anchor behind and silently create a selection, which is the trap
/// worth naming: HasSelection() is anchor != cursor.
[[nodiscard]] auto CaretAt(size_t pos) noexcept -> Caret {
    Caret c {};
    c.cursorIndex = static_cast<uint32_t>(pos);
    c.ClearSelection();
    return c;
}

} // namespace

struct TextEditTestSuite {
    struct Tests {
        std::expected<void, ZHLN::Error> basic_typing_and_navigation() {
            std::string text;
            Caret       caret;

            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::HandleChar(text, caret, 'H'));
            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::HandleChar(text, caret, 'i'));

            ZHLN::Test::ExpectEq(std::string_view(text), std::string_view("Hi"));
            ZHLN::Test::ExpectEq(caret.cursorIndex, 2u);

            Modifiers mods {};
            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::Left, mods), ZHLN::GUI::TextEdit::KeyResult::Navigated
            );
            ZHLN::Test::ExpectEq(caret.cursorIndex, 1u);

            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::Backspace, mods), ZHLN::GUI::TextEdit::KeyResult::Edited
            );
            ZHLN::Test::ExpectEq(std::string_view(text), std::string_view("i"));
            ZHLN::Test::ExpectEq(caret.cursorIndex, 0u);

            // Anything outside printable ASCII is dropped: the font atlas covers
            // 32..126 and a '\n' would break the single-line model.
            ZHLN::Test::ExpectFalse(ZHLN::GUI::TextEdit::HandleChar(text, caret, '\n'));
            ZHLN::Test::ExpectFalse(ZHLN::GUI::TextEdit::HandleChar(text, caret, 0x20AC)); // euro sign
            ZHLN::Test::ExpectEq(std::string_view(text), std::string_view("i"));

            return {};
        }

        std::expected<void, ZHLN::Error> text_selection_and_replacement() {
            std::string text {"Hello World"};
            Caret       caret = CaretAt(5); // After "Hello"

            Modifiers shift {.shift = true};
            for (int i = 0; i < 5; ++i) {
                ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::Left, shift);
            }

            ZHLN::Test::ExpectTrue(caret.HasSelection());
            ZHLN::Test::ExpectEq(caret.SelectionStart(), 0u);
            ZHLN::Test::ExpectEq(caret.SelectionEnd(text.size()), 5u);
            ZHLN::Test::ExpectEq(ZHLN::GUI::TextEdit::SelectedText(text, caret), std::string_view("Hello"));

            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::HandleChar(text, caret, 'B'));
            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::HandleChar(text, caret, 'y'));
            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::HandleChar(text, caret, 'e'));

            ZHLN::Test::ExpectEq(std::string_view(text), std::string_view("Bye World"));
            ZHLN::Test::ExpectFalse(caret.HasSelection());

            // An un-shifted move collapses onto the edge the key implies, rather
            // than stepping from wherever the caret happened to be.
            caret = CaretAt(3);
            Modifiers mods {};
            ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::End, shift);
            ZHLN::Test::ExpectEq(caret.SelectionStart(), 3u);
            ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::Left, mods);
            ZHLN::Test::ExpectFalse(caret.HasSelection());
            ZHLN::Test::ExpectEq(caret.cursorIndex, 3u);

            return {};
        }

        std::expected<void, ZHLN::Error> word_navigation_and_deletion() {
            std::string text {"alpha beta gamma"};
            Caret       caret = CaretAt(text.size());

            Modifiers ctrl {.ctrl = true};

            // Ctrl+Left walks back over "gamma", Ctrl+Right forward again.
            ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::Left, ctrl);
            ZHLN::Test::ExpectEq(caret.cursorIndex, 11u);
            ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::Right, ctrl);
            ZHLN::Test::ExpectEq(caret.cursorIndex, 16u);

            // Ctrl+Backspace deletes the whole word, not one character.
            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::Backspace, ctrl), ZHLN::GUI::TextEdit::KeyResult::Edited
            );
            ZHLN::Test::ExpectEq(std::string_view(text), std::string_view("alpha beta "));

            // Home/End bound the caret, and deleting at a boundary is a no-op
            // that must not report an edit.
            ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::Home, ctrl);
            ZHLN::Test::ExpectEq(caret.cursorIndex, 0u);
            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::Backspace, ctrl),
                ZHLN::GUI::TextEdit::KeyResult::Navigated
            );
            ZHLN::Test::ExpectEq(std::string_view(text), std::string_view("alpha beta "));

            return {};
        }

        std::expected<void, ZHLN::Error> clipboard_exchange() {
            std::string text {"alpha beta gamma"};
            Caret       caret = CaretAt(text.size());

            std::string clipboardText;
            ZHLN::GUI::TextEdit::ClipboardSink clipboard {
                .userdata = &clipboardText,
                .set      = [](void* ud, std::string_view t) -> void { *static_cast<std::string*>(ud) = t; },
                .get      = [](void* ud) -> std::string { return *static_cast<std::string*>(ud); },
            };

            Modifiers mods {};
            Modifiers ctrl {.ctrl = true};

            ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::A, ctrl);
            ZHLN::Test::ExpectTrue(caret.HasSelection());
            ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::C, ctrl, clipboard);
            ZHLN::Test::ExpectEq(clipboardText, std::string("alpha beta gamma"));

            // Without Ctrl the same keys are not text-editing keys at all, so
            // they stay free for gameplay hotkeys.
            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::C, mods), ZHLN::GUI::TextEdit::KeyResult::Ignored
            );
            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::V, mods), ZHLN::GUI::TextEdit::KeyResult::Ignored
            );

            ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::A, ctrl);
            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::X, ctrl, clipboard),
                ZHLN::GUI::TextEdit::KeyResult::Edited
            );
            ZHLN::Test::ExpectTrue(text.empty());
            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::V, ctrl, clipboard),
                ZHLN::GUI::TextEdit::KeyResult::Edited
            );
            ZHLN::Test::ExpectEq(std::string_view(text), std::string_view("alpha beta gamma"));

            // An empty sink must not crash or edit: Ctrl+C/X/V simply do nothing.
            std::string other {"keep"};
            Caret       oc = CaretAt(other.size());
            ZHLN::GUI::TextEdit::HandleKey(other, oc, ZHLN::KeyCode::A, ctrl);
            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(other, oc, ZHLN::KeyCode::C, ctrl), ZHLN::GUI::TextEdit::KeyResult::Navigated
            );
            ZHLN::Test::ExpectEq(std::string_view(other), std::string_view("keep"));

            return {};
        }

        std::expected<void, ZHLN::Error> fixed_capacity_paste_shrinks_and_keeps_the_tail() {
            // A String32 holds 31 characters. Pasting 26 at offset 5 leaves room
            // for 21, so the PASTE has to be the thing that gives way -- letting
            // assign() clamp instead would keep the whole paste and eat "56789".
            ZHLN::String32 text {"0123456789"};
            Caret          caret = CaretAt(5);

            ZHLN::Test::ExpectEq(ZHLN::GUI::TextEdit::MaxTextLength(text), 31u);
            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::InsertText(text, caret, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"));

            ZHLN::Test::ExpectEq(std::string_view(text), std::string_view("01234ABCDEFGHIJKLMNOPQRSTU56789"));
            ZHLN::Test::ExpectEq(text.size(), 31u);
            ZHLN::Test::ExpectEq(caret.cursorIndex, 26u); // Parked after what was actually inserted

            // The same edit against an unbounded store keeps all 26.
            std::string grown {"0123456789"};
            Caret       gc = CaretAt(5);
            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::InsertText(grown, gc, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"));
            ZHLN::Test::ExpectEq(std::string_view(grown), std::string_view("01234ABCDEFGHIJKLMNOPQRSTUVWXYZ56789"));
            ZHLN::Test::ExpectEq(grown.size(), 36u);
            ZHLN::Test::ExpectEq(gc.cursorIndex, 31u);

            return {};
        }

        std::expected<void, ZHLN::Error> bounded_scratch_applies_a_fixed_limit_to_a_std_string() {
            // Context::TextInput edits a std::string scratch on behalf of a
            // fixed-capacity caller, so the limit has to travel with the scratch
            // rather than being rediscovered from the store.
            std::string                        backing {"0123456789"};
            ZHLN::GUI::TextEdit::BoundedString buf {&backing, 15};
            Caret                              caret = CaretAt(5);

            ZHLN::Test::ExpectEq(ZHLN::GUI::TextEdit::MaxTextLength(buf), 15u);
            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::InsertText(buf, caret, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"));

            ZHLN::Test::ExpectEq(backing.size(), 15u);
            ZHLN::Test::ExpectEq(std::string_view(backing), std::string_view("01234ABCDE56789"));
            ZHLN::Test::ExpectEq(caret.cursorIndex, 10u);

            return {};
        }

        std::expected<void, ZHLN::Error> commit_clears_the_selection_but_not_the_text() {
            std::string text {"Name"};
            Caret       caret {};
            caret.selectAll = true;

            Modifiers mods {};
            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::Enter, mods), ZHLN::GUI::TextEdit::KeyResult::Committed
            );
            ZHLN::Test::ExpectFalse(caret.HasSelection());
            ZHLN::Test::ExpectEq(std::string_view(text), std::string_view("Name"));

            // Escape behaves the same way. Releasing focus is the caller's job:
            // where focus lives differs per front end, and Context::TextInput
            // keeps it in its own widget table.
            caret.selectAll = true;
            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::Escape, mods), ZHLN::GUI::TextEdit::KeyResult::Committed
            );
            ZHLN::Test::ExpectFalse(caret.HasSelection());

            return {};
        }
    };
};

auto RunTextEditSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<TextEditTestSuite>();
}
