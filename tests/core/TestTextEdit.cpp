// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/String.hpp>
#include <Zahlen/gui/TextBuffer.hpp>
#include <Zahlen/gui/TextEdit.hpp>
#include <Zahlen/gui/UIComponents.hpp>
#include <expected>
#include <string>
#include <string_view>

enum class TextEditTestError : uint8_t {
    NavigationFailed ZHLN_ANNOTATION(ZHLN::Description<"Caret navigation behaved incorrectly."> {}) = 1,
    EditFailed       ZHLN_ANNOTATION(ZHLN::Description<"Text insertion or deletion behaved incorrectly."> {}),
    SelectionFailed  ZHLN_ANNOTATION(ZHLN::Description<"Text selection logic behaved incorrectly."> {}),
};

struct TextEditTestSuite {
    struct Tests {
        std::expected<void, ZHLN::Error> basic_typing_and_navigation() {
            ZHLN::GUI::UIComponents::UITextInputComponent in;

            // Typing characters
            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::HandleChar(in, 'H'));
            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::HandleChar(in, 'i'));

            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("Hi"));
            ZHLN::Test::ExpectEq(in.caret.cursorIndex, 2u);

            // Left navigation
            ZHLN::GUI::TextEdit::Modifiers mods {};
            ZHLN::Test::ExpectEq(ZHLN::GUI::TextEdit::HandleKey(in, ZHLN::KeyCode::Left, mods), ZHLN::GUI::TextEdit::KeyResult::Navigated);
            ZHLN::Test::ExpectEq(in.caret.cursorIndex, 1u);

            // Backspace
            ZHLN::Test::ExpectEq(ZHLN::GUI::TextEdit::HandleKey(in, ZHLN::KeyCode::Backspace, mods), ZHLN::GUI::TextEdit::KeyResult::Edited);
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("i"));
            ZHLN::Test::ExpectEq(in.caret.cursorIndex, 0u);

            return {};
        }

        std::expected<void, ZHLN::Error> text_selection_and_replacement() {
            ZHLN::GUI::UIComponents::UITextInputComponent in;
            in.text        = "Hello World";
            in.caret.cursorIndex = 5; // Set cursor after "Hello"
            in.ClearSelection();

            ZHLN::GUI::TextEdit::Modifiers mods {.shift = true};

            // Shift + Left 5 times
            for (int i = 0; i < 5; ++i) {
                ZHLN::GUI::TextEdit::HandleKey(in, ZHLN::KeyCode::Left, mods);
            }

            ZHLN::Test::ExpectTrue(in.HasSelection());
            ZHLN::Test::ExpectEq(in.SelectionStart(), 0u);
            ZHLN::Test::ExpectEq(in.SelectionEnd(), 5u);
            ZHLN::Test::ExpectEq(ZHLN::GUI::TextEdit::SelectedText(in), std::string_view("Hello"));

            // Replace Selection
            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::HandleChar(in, 'B'));
            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::HandleChar(in, 'y'));
            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::HandleChar(in, 'e'));

            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("Bye World"));
            ZHLN::Test::ExpectFalse(in.HasSelection());

            return {};
        }

        // The editing rules are shared with the immediate-mode Context::TextInput
        // through Zahlen/gui/TextBuffer.hpp. These next tests drive that core
        // directly over storages the ECS component never was, which is the whole
        // point of having extracted it: if the core ever grows a hidden
        // dependency on UITextInputComponent, these stop compiling.
        std::expected<void, ZHLN::Error> core_edits_any_buffer() {
            std::string                    text;
            ZHLN::GUI::TextEdit::Caret     caret;

            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::HandleChar(text, caret, 'H'));
            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::HandleChar(text, caret, 'i'));
            ZHLN::Test::ExpectEq(std::string_view(text), std::string_view("Hi"));
            ZHLN::Test::ExpectEq(caret.cursorIndex, 2u);

            ZHLN::GUI::TextEdit::Modifiers mods {};
            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::Backspace, mods), ZHLN::GUI::TextEdit::KeyResult::Edited
            );
            ZHLN::Test::ExpectEq(std::string_view(text), std::string_view("H"));

            // Shift+End selects to the end, then a character replaces it.
            ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::Home, mods);
            ZHLN::GUI::TextEdit::Modifiers shift {.shift = true};
            ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::End, shift);
            ZHLN::Test::ExpectTrue(caret.HasSelection());
            ZHLN::Test::ExpectEq(ZHLN::GUI::TextEdit::SelectedText(text, caret), std::string_view("H"));
            ZHLN::GUI::TextEdit::HandleChar(text, caret, 'X');
            ZHLN::Test::ExpectEq(std::string_view(text), std::string_view("X"));
            ZHLN::Test::ExpectFalse(caret.HasSelection());

            return {};
        }

        std::expected<void, ZHLN::Error> fixed_capacity_paste_shrinks_and_keeps_the_tail() {
            // A String32 holds 31 characters. Pasting 26 at offset 5 leaves room
            // for 21, so the PASTE has to be the thing that gives way -- letting
            // assign() clamp instead would keep the whole paste and eat "56789".
            ZHLN::String32             text {"0123456789"};
            ZHLN::GUI::TextEdit::Caret caret {};
            caret.cursorIndex = 5;
            caret.ClearSelection();

            ZHLN::Test::ExpectEq(ZHLN::GUI::TextEdit::MaxTextLength(text), 31u);
            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::InsertText(text, caret, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"));

            ZHLN::Test::ExpectEq(std::string_view(text), std::string_view("01234ABCDEFGHIJKLMNOPQRSTU56789"));
            ZHLN::Test::ExpectEq(text.size(), 31u);
            ZHLN::Test::ExpectEq(caret.cursorIndex, 26u); // Parked after what was actually inserted

            // The same edit against an unbounded store keeps all 26.
            std::string                grown {"0123456789"};
            ZHLN::GUI::TextEdit::Caret gc {};
            gc.cursorIndex = 5;
            gc.ClearSelection();
            ZHLN::Test::ExpectTrue(ZHLN::GUI::TextEdit::InsertText(grown, gc, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"));
            ZHLN::Test::ExpectEq(std::string_view(grown), std::string_view("01234ABCDEFGHIJKLMNOPQRSTUVWXYZ56789"));
            ZHLN::Test::ExpectEq(grown.size(), 36u);
            ZHLN::Test::ExpectEq(gc.cursorIndex, 31u);

            return {};
        }

        std::expected<void, ZHLN::Error> word_navigation_and_clipboard_exchange() {
            std::string                text {"alpha beta gamma"};
            ZHLN::GUI::TextEdit::Caret caret {};
            caret.cursorIndex = 16;
            caret.ClearSelection();

            std::string clipboardText;
            ZHLN::GUI::TextEdit::ClipboardSink clipboard {
                .userdata = &clipboardText,
                .set      = [](void* ud, std::string_view t) -> void { *static_cast<std::string*>(ud) = t; },
                .get      = [](void* ud) -> std::string { return *static_cast<std::string*>(ud); },
            };

            ZHLN::GUI::TextEdit::Modifiers mods {};
            ZHLN::GUI::TextEdit::Modifiers ctrl {.ctrl = true};

            // Ctrl+Left walks back over "gamma".
            ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::Left, ctrl);
            ZHLN::Test::ExpectEq(caret.cursorIndex, 11u);

            // Ctrl+A selects everything; Ctrl+C copies it.
            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::A, ctrl), ZHLN::GUI::TextEdit::KeyResult::Navigated
            );
            ZHLN::Test::ExpectTrue(caret.HasSelection());
            ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::C, ctrl, clipboard);
            ZHLN::Test::ExpectEq(clipboardText, std::string("alpha beta gamma"));

            // Without Ctrl the same keys are not text-editing keys at all, so
            // they stay free for gameplay hotkeys.
            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::C, mods), ZHLN::GUI::TextEdit::KeyResult::Ignored
            );

            // Ctrl+X cuts the selection, Ctrl+V puts it back.
            ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::A, ctrl);
            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::X, ctrl, clipboard), ZHLN::GUI::TextEdit::KeyResult::Edited
            );
            ZHLN::Test::ExpectTrue(text.empty());
            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(text, caret, ZHLN::KeyCode::V, ctrl, clipboard), ZHLN::GUI::TextEdit::KeyResult::Edited
            );
            ZHLN::Test::ExpectEq(std::string_view(text), std::string_view("alpha beta gamma"));

            return {};
        }

        std::expected<void, ZHLN::Error> commit_clears_the_selection_and_releases_focus() {
            ZHLN::GUI::UIComponents::UITextInputComponent in;
            in.text        = "Name";
            in.isFocused   = true;
            in.caret.selectAll = true;

            ZHLN::GUI::TextEdit::Modifiers mods {};
            ZHLN::Test::ExpectEq(
                ZHLN::GUI::TextEdit::HandleKey(in, ZHLN::KeyCode::Enter, mods), ZHLN::GUI::TextEdit::KeyResult::Committed
            );
            // The core clears the selection; focus is the component's field, so
            // the adapter is what has to drop it.
            ZHLN::Test::ExpectFalse(in.HasSelection());
            ZHLN::Test::ExpectFalse(in.isFocused);
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("Name"));

            return {};
        }
    };
};

auto RunTextEditSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<TextEditTestSuite>();
}
