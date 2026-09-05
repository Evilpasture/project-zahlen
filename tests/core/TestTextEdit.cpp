// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/core/TestTextEdit.cpp
//
// Behavioural tests for GUI::TextEdit, the keyboard model behind
// ui.TextInput: caret movement (including word jumps and Home/End),
// selection by Shift + navigation and Ctrl+A, replacement of the selection by
// typing/pasting, Backspace/Delete semantics and the clipboard exchange.
//
// Everything here drives the same functions Engine's onKey/onChar callbacks
// forward to, on a bare UITextInputComponent: no registry, no window, no
// renderer. The clipboard is a std::string behind the ClipboardSink
// interface, which is exactly how the engine adapts Window's clipboard.

#include "TestsFramework.hpp"
#include <Zahlen/Input.hpp>
#include <Zahlen/gui/TextEdit.hpp>
#include <Zahlen/gui/UIComponents.hpp>
#include <expected>
#include <string>
#include <string_view>

namespace {

namespace TE   = ZHLN::GUI::TextEdit;
using Input    = ZHLN::GUI::UIComponents::UITextInputComponent;
using KeyCode  = ZHLN::KeyCode;
constexpr TE::Modifiers kNone  = {};
constexpr TE::Modifiers kShift = {.shift = true};
constexpr TE::Modifiers kCtrl  = {.ctrl = true};

// A focused field with the caret at the end and nothing selected, the state
// the interaction system leaves after a click that did not select-all.
auto MakeInput(std::string_view text) -> Input {
    Input in {};
    in.text.assign(text);
    in.cursorIndex     = static_cast<uint32_t>(text.size());
    in.selectionAnchor = in.cursorIndex;
    in.isFocused       = true;
    return in;
}

// Clipboard stand-in: the engine adapts Window::Get/SetClipboardText the
// same way, so the tests see the exact sink contract the engine uses.
struct FakeClipboard {
    std::string contents;

    [[nodiscard]] auto Sink() -> TE::ClipboardSink {
        return TE::ClipboardSink {
            .userdata = this,
            .set      = [](void* ud, std::string_view t) -> void { static_cast<FakeClipboard*>(ud)->contents.assign(t); },
            .get      = [](void* ud) -> std::string { return static_cast<FakeClipboard*>(ud)->contents; },
        };
    }
};

void Type(Input& in, std::string_view s) {
    for (char c: s) {
        TE::HandleChar(in, static_cast<unsigned int>(static_cast<unsigned char>(c)));
    }
}

} // namespace

struct TextEditTestSuite {
    struct Tests {
        // ==================================================================
        // TYPING AND DELETION
        // ==================================================================

        auto typing_inserts_at_the_caret_and_flags_edited() -> std::expected<void, ZHLN::Error> {
            Input in = MakeInput("helo");
            in.cursorIndex = in.selectionAnchor = 3;
            in.edited                           = false;

            ZHLN::Test::ExpectTrue(TE::HandleChar(in, 'l'));
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("hello"));
            ZHLN::Test::ExpectEq(in.cursorIndex, 4u);
            ZHLN::Test::ExpectTrue(in.edited);
            ZHLN::Test::ExpectFalse(in.HasSelection());

            // Non-printable codepoints are refused and leave the text alone.
            in.edited = false;
            ZHLN::Test::ExpectFalse(TE::HandleChar(in, '\n'));
            ZHLN::Test::ExpectFalse(TE::HandleChar(in, 0x00E9)); // e-acute: outside the atlas
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("hello"));
            ZHLN::Test::ExpectFalse(in.edited);
            return {};
        }

        auto backspace_and_delete_remove_one_character_each_side() -> std::expected<void, ZHLN::Error> {
            Input in = MakeInput("abcd");
            in.cursorIndex = in.selectionAnchor = 2;

            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::Backspace, kNone) == TE::KeyResult::Edited);
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("acd"));
            ZHLN::Test::ExpectEq(in.cursorIndex, 1u);

            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::Delete, kNone) == TE::KeyResult::Edited);
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("ad"));
            ZHLN::Test::ExpectEq(in.cursorIndex, 1u);

            // At the edges the keys are accepted but change nothing.
            in.cursorIndex = in.selectionAnchor = 0;
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::Backspace, kNone) == TE::KeyResult::Navigated);
            in.cursorIndex = in.selectionAnchor = 2;
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::Delete, kNone) == TE::KeyResult::Navigated);
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("ad"));
            return {};
        }

        auto ctrl_backspace_deletes_the_previous_word() -> std::expected<void, ZHLN::Error> {
            Input in = MakeInput("alpha beta_2  gamma");

            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::Backspace, kCtrl) == TE::KeyResult::Edited);
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("alpha beta_2  "));

            // Separators between the caret and the word go with it.
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::Backspace, kCtrl) == TE::KeyResult::Edited);
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("alpha "));

            in.cursorIndex = in.selectionAnchor = 0;
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::Delete, kCtrl) == TE::KeyResult::Edited);
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view(" "));
            return {};
        }

        auto text_is_clamped_to_the_fixed_string_capacity() -> std::expected<void, ZHLN::Error> {
            Input in = MakeInput(std::string(250, 'x'));
            Type(in, "abcdefgh");
            // String256 holds 255 characters plus the terminator.
            ZHLN::Test::ExpectEq(in.text.size(), std::size_t {255});
            ZHLN::Test::ExpectEq(in.cursorIndex, 255u);
            ZHLN::Test::ExpectEq(std::string_view(in.text).substr(250), std::string_view("abcde"));
            return {};
        }

        // ==================================================================
        // NAVIGATION
        // ==================================================================

        auto home_end_and_arrows_move_the_caret_and_collapse_selection() -> std::expected<void, ZHLN::Error> {
            Input in = MakeInput("hello world");

            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::Home, kNone) == TE::KeyResult::Navigated);
            ZHLN::Test::ExpectEq(in.cursorIndex, 0u);
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::Right, kNone) == TE::KeyResult::Navigated);
            ZHLN::Test::ExpectEq(in.cursorIndex, 1u);
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::End, kNone) == TE::KeyResult::Navigated);
            ZHLN::Test::ExpectEq(in.cursorIndex, 11u);
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::Left, kNone) == TE::KeyResult::Navigated);
            ZHLN::Test::ExpectEq(in.cursorIndex, 10u);
            ZHLN::Test::ExpectFalse(in.HasSelection());

            // Focus-gain selects everything; a plain arrow collapses that to
            // the matching edge rather than nudging the caret by one.
            in.selectAll = true;
            TE::HandleKey(in, KeyCode::Left, kNone);
            ZHLN::Test::ExpectEq(in.cursorIndex, 0u);
            ZHLN::Test::ExpectFalse(in.HasSelection());
            in.selectAll = true;
            TE::HandleKey(in, KeyCode::Right, kNone);
            ZHLN::Test::ExpectEq(in.cursorIndex, 11u);
            ZHLN::Test::ExpectFalse(in.HasSelection());
            return {};
        }

        auto ctrl_arrows_jump_by_word() -> std::expected<void, ZHLN::Error> {
            Input in = MakeInput("foo.bar  baz_qux");
            TE::HandleKey(in, KeyCode::Home, kNone);

            TE::HandleKey(in, KeyCode::Right, kCtrl);
            ZHLN::Test::ExpectEq(in.cursorIndex, 3u); // after "foo"
            TE::HandleKey(in, KeyCode::Right, kCtrl);
            ZHLN::Test::ExpectEq(in.cursorIndex, 7u); // after "bar"
            TE::HandleKey(in, KeyCode::Right, kCtrl);
            ZHLN::Test::ExpectEq(in.cursorIndex, 16u); // underscores are word characters
            TE::HandleKey(in, KeyCode::Right, kCtrl);
            ZHLN::Test::ExpectEq(in.cursorIndex, 16u); // pinned at the end

            TE::HandleKey(in, KeyCode::Left, kCtrl);
            ZHLN::Test::ExpectEq(in.cursorIndex, 9u); // start of "baz_qux"
            TE::HandleKey(in, KeyCode::Left, kCtrl);
            ZHLN::Test::ExpectEq(in.cursorIndex, 4u); // start of "bar"
            TE::HandleKey(in, KeyCode::Left, kCtrl);
            ZHLN::Test::ExpectEq(in.cursorIndex, 0u);
            return {};
        }

        // ==================================================================
        // SELECTION
        // ==================================================================

        auto shift_navigation_extends_a_selection_from_the_anchor() -> std::expected<void, ZHLN::Error> {
            Input in = MakeInput("hello world");
            in.cursorIndex = in.selectionAnchor = 5;

            TE::HandleKey(in, KeyCode::Left, kShift);
            TE::HandleKey(in, KeyCode::Left, kShift);
            ZHLN::Test::ExpectTrue(in.HasSelection());
            ZHLN::Test::ExpectEq(in.SelectionStart(), 3u);
            ZHLN::Test::ExpectEq(in.SelectionEnd(), 5u);
            ZHLN::Test::ExpectEq(TE::SelectedText(in), std::string_view("lo"));

            // Crossing the anchor flips the range around it.
            TE::HandleKey(in, KeyCode::End, kShift);
            ZHLN::Test::ExpectEq(TE::SelectedText(in), std::string_view(" world"));

            TE::HandleKey(in, KeyCode::Home, kShift);
            ZHLN::Test::ExpectEq(TE::SelectedText(in), std::string_view("hello"));

            // Shift+Ctrl+Right selects word-wise.
            TE::HandleKey(in, KeyCode::Right, TE::Modifiers {.shift = true, .ctrl = true});
            ZHLN::Test::ExpectEq(in.cursorIndex, 5u);
            ZHLN::Test::ExpectFalse(in.HasSelection()); // back on the anchor
            TE::HandleKey(in, KeyCode::Right, TE::Modifiers {.shift = true, .ctrl = true});
            ZHLN::Test::ExpectEq(TE::SelectedText(in), std::string_view(" world"));
            return {};
        }

        auto ctrl_a_selects_everything_and_typing_replaces_it() -> std::expected<void, ZHLN::Error> {
            Input in = MakeInput("Default");
            in.cursorIndex = in.selectionAnchor = 3;

            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::A, kCtrl) == TE::KeyResult::Navigated);
            ZHLN::Test::ExpectTrue(in.HasSelection());
            ZHLN::Test::ExpectEq(TE::SelectedText(in), std::string_view("Default"));

            // Plain 'a' without Ctrl is not a hotkey: the char path types it.
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::A, kNone) == TE::KeyResult::Ignored);

            Type(in, "Hi");
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("Hi"));
            ZHLN::Test::ExpectEq(in.cursorIndex, 2u);
            ZHLN::Test::ExpectFalse(in.HasSelection());
            ZHLN::Test::ExpectTrue(in.edited);
            return {};
        }

        auto backspace_removes_the_whole_selection() -> std::expected<void, ZHLN::Error> {
            Input in = MakeInput("hello world");
            in.cursorIndex     = 5;
            in.selectionAnchor = 11;
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::Backspace, kNone) == TE::KeyResult::Edited);
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("hello"));
            ZHLN::Test::ExpectEq(in.cursorIndex, 5u);
            ZHLN::Test::ExpectFalse(in.HasSelection());

            // The focus-gain flag behaves like a full selection.
            in.selectAll = true;
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::Delete, kNone) == TE::KeyResult::Edited);
            ZHLN::Test::ExpectTrue(in.text.empty());
            ZHLN::Test::ExpectEq(in.cursorIndex, 0u);
            return {};
        }

        // ==================================================================
        // CLIPBOARD
        // ==================================================================

        auto copy_cut_and_paste_round_trip_through_the_clipboard() -> std::expected<void, ZHLN::Error> {
            FakeClipboard clip;
            Input         in = MakeInput("hello world");

            // Copy with nothing selected leaves the clipboard alone.
            TE::HandleKey(in, KeyCode::C, kCtrl, clip.Sink());
            ZHLN::Test::ExpectTrue(clip.contents.empty());

            in.cursorIndex     = 0;
            in.selectionAnchor = 5;
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::C, kCtrl, clip.Sink()) == TE::KeyResult::Navigated);
            ZHLN::Test::ExpectEq(clip.contents, std::string("hello"));
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("hello world")); // copy does not edit
            ZHLN::Test::ExpectTrue(in.HasSelection());                                        // ... or deselect

            TE::HandleKey(in, KeyCode::End, kNone);
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::V, kCtrl, clip.Sink()) == TE::KeyResult::Edited);
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("hello worldhello"));
            ZHLN::Test::ExpectEq(in.cursorIndex, 16u);

            // Cut removes the selection and stores it.
            in.cursorIndex     = 11;
            in.selectionAnchor = 16;
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::X, kCtrl, clip.Sink()) == TE::KeyResult::Edited);
            ZHLN::Test::ExpectEq(clip.contents, std::string("hello"));
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("hello world"));
            ZHLN::Test::ExpectEq(in.cursorIndex, 11u);

            // Paste over a selection replaces it, and multi-line clipboard
            // content is flattened to what a single-line field can show.
            clip.contents      = "big\nbad";
            in.cursorIndex     = 0;
            in.selectionAnchor = 5;
            TE::HandleKey(in, KeyCode::V, kCtrl, clip.Sink());
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("bigbad world"));
            ZHLN::Test::ExpectEq(in.cursorIndex, 6u);

            // Without a sink the hotkeys are inert but still consumed.
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::V, kCtrl) == TE::KeyResult::Navigated);
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("bigbad world"));
            return {};
        }

        // ==================================================================
        // COMMIT
        // ==================================================================

        auto enter_and_escape_release_focus_and_keep_the_text() -> std::expected<void, ZHLN::Error> {
            Input in = MakeInput("keep me");
            in.selectAll = true;
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::Enter, kNone) == TE::KeyResult::Committed);
            ZHLN::Test::ExpectFalse(in.isFocused);
            ZHLN::Test::ExpectFalse(in.HasSelection());
            ZHLN::Test::ExpectEq(std::string_view(in.text), std::string_view("keep me"));

            in.isFocused = true;
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::Escape, kNone) == TE::KeyResult::Committed);
            ZHLN::Test::ExpectFalse(in.isFocused);

            // Keys the field does not handle are reported as such, so the
            // caller may route them elsewhere.
            ZHLN::Test::ExpectTrue(TE::HandleKey(in, KeyCode::F5, kNone) == TE::KeyResult::Ignored);
            return {};
        }
    };
};

// Exported for the core group binary (RunCoreTests.cpp), which aggregates
// every suite in this directory through Runner::RunDeferred.
auto RunTextEditSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<TextEditTestSuite>();
}
