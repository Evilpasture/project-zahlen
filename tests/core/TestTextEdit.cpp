// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/gui/TextEdit.hpp>
#include <Zahlen/gui/UIComponents.hpp>
#include <expected>
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
            ZHLN::Test::ExpectEq(in.cursorIndex, 2u);

            // Left navigation
            ZHLN::GUI::TextEdit::Modifiers mods {};
            ZHLN::Test::ExpectEq(ZHLN::GUI::TextEdit::HandleKey(in, ZHLN::KeyCode::Left, mods), ZHLN::GUI::TextEdit::KeyResult::Navigated);
            ZHLN::Test::ExpectEq(in.cursorIndex, 1u);

            // Backspace
            ZHLN::Test::ExpectEq(ZHLN::GUI::TextEdit::HandleKey(in, ZHLN::KeyCode::Backspace, mods), ZHLN::GUI::TextEdit::KeyResult::Edited);
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("i"));
            ZHLN::Test::ExpectEq(in.cursorIndex, 0u);

            return {};
        }

        std::expected<void, ZHLN::Error> text_selection_and_replacement() {
            ZHLN::GUI::UIComponents::UITextInputComponent in;
            in.text        = "Hello World";
            in.cursorIndex = 5; // Set cursor after "Hello"

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
    };
};

auto RunTextEditSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<TextEditTestSuite>();
}
