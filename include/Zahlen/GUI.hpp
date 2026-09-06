// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Common.h>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/gui/TextBuffer.hpp>
#include <concepts>
#include <memory>
#include <string>
#include <string_view>

namespace ZHLN {
class Engine;
class RenderContext;
namespace ECS {
class Registry;
}
} // namespace ZHLN

namespace ZHLN::GUI {

enum class Direction : uint8_t { Row, Column };
enum class Alignment : uint8_t { Start, Center, End, Stretch };

struct Sizing {
    float fixed = 0.0f; // > 0: exact pixels
    float grow  = 0.0f; // > 0: flex-grow ratio
    bool  fit   = true; // fit content
};

struct BoxConfig {
    Sizing    width        = {};
    Sizing    height       = {};
    JPH::Vec4 color        = {0.0f, 0.0f, 0.0f, 0.0f};
    JPH::Vec4 cornerRadius = {0.0f, 0.0f, 0.0f, 0.0f};
    float     padding      = 0.0f;
    float     gap          = 0.0f;
    Direction direction    = Direction::Column;
    Alignment alignMain    = Alignment::Start;
    Alignment alignCross   = Alignment::Start;
};

class ZHLN_API Context {
  public:
    struct Impl;

    explicit Context(Engine& engine) noexcept;
    explicit Context(ECS::Registry& registry, Extent2D viewport = {1920, 1080}) noexcept;
    ~Context() noexcept;

    Context(const Context&)            = default;
    Context& operator=(const Context&) = default;
    Context(Context&&) noexcept        = default;
    Context& operator=(Context&&) noexcept = default;

    // --- Frame Lifecycle ---
    void BeginFrame(float dt) noexcept;
    void EndFrame() noexcept;
    void EndFrameAndRender(RenderContext& rc) noexcept;

    // --- Layout Containers (Macro-free C++ API) ---
    void BeginBox(std::string_view id, const BoxConfig& cfg = {}) noexcept;
    void EndBox() noexcept;

    void BeginRow(float gap = 0.0f, float padding = 0.0f) noexcept;
    void EndRow() noexcept;

    void BeginColumn(float gap = 0.0f, float padding = 0.0f) noexcept;
    void EndColumn() noexcept;

    // Closures
    template <typename Fn>
        requires std::invocable<Fn>
    void Box(std::string_view id, const BoxConfig& cfg, Fn&& content) {
        BeginBox(id, cfg);
        content();
        EndBox();
    }

    template <typename Fn>
        requires std::invocable<Fn>
    void Row(float gap, Fn&& content) {
        BeginRow(gap);
        content();
        EndRow();
    }

    template <typename Fn>
        requires std::invocable<Fn>
    void Column(float gap, Fn&& content) {
        BeginColumn(gap);
        content();
        EndColumn();
    }

    // --- Interactive Widgets ---
    void Text(std::string_view text, float fontSize = 16.0f, const JPH::Vec4& color = {1, 1, 1, 1}) noexcept;
    bool Button(std::string_view label, const JPH::Vec4& color = {0.16f, 0.24f, 0.36f, 0.95f}, const Sizing& width = {}) noexcept;
    bool Button(std::string_view label, const Sizing& width) noexcept {
        return Button(label, {0.16f, 0.24f, 0.36f, 0.95f}, width);
    }

    template <typename OnClickFn>
        requires std::invocable<OnClickFn>
    bool Button(std::string_view label, OnClickFn&& onClick) {
        if (Button(label)) {
            onClick();
            return true;
        }
        return false;
    }

    template <typename OnClickFn, typename OnHoverFn>
        requires std::invocable<OnClickFn> && std::invocable<OnHoverFn>
    bool Button(std::string_view label, OnClickFn&& onClick, OnHoverFn&& onHover) {
        bool clicked = Button(label);
        if (IsItemHovered()) {
            onHover();
        }
        if (clicked) {
            onClick();
        }
        return clicked;
    }

    template <typename OnClickFn>
        requires std::invocable<OnClickFn>
    bool Button(std::string_view label, const Sizing& width, OnClickFn&& onClick) {
        if (Button(label, width)) {
            onClick();
            return true;
        }
        return false;
    }

    template <typename OnClickFn, typename OnHoverFn>
        requires std::invocable<OnClickFn> && std::invocable<OnHoverFn>
    bool Button(std::string_view label, const Sizing& width, OnClickFn&& onClick, OnHoverFn&& onHover) {
        bool clicked = Button(label, width);
        if (IsItemHovered()) {
            onHover();
        }
        if (clicked) {
            onClick();
        }
        return clicked;
    }

    template <typename OnClickFn>
        requires std::invocable<OnClickFn>
    bool Button(std::string_view label, const JPH::Vec4& color, const Sizing& width, OnClickFn&& onClick) {
        if (Button(label, color, width)) {
            onClick();
            return true;
        }
        return false;
    }

    template <typename OnClickFn, typename OnHoverFn>
        requires std::invocable<OnClickFn> && std::invocable<OnHoverFn>
    bool Button(std::string_view label, const JPH::Vec4& color, const Sizing& width, OnClickFn&& onClick, OnHoverFn&& onHover) {
        bool clicked = Button(label, color, width);
        if (IsItemHovered()) {
            onHover();
        }
        if (clicked) {
            onClick();
        }
        return clicked;
    }

    // --- State Inspection ---
    [[nodiscard]] bool IsItemHovered() const noexcept;
    [[nodiscard]] bool IsItemActive() const noexcept;

    bool Checkbox(std::string_view label, bool& checked) noexcept;
    bool Slider(std::string_view label, float& value, float minVal, float maxVal) noexcept;

    // --- Text Input ---
    //
    // Single-line editable field. Returns true on any frame the text changed.
    // Editing is done by the same rules as the ECS UITextInputComponent
    // (Zahlen/gui/TextBuffer.hpp), so caret movement, selection, word deletion
    // and Ctrl+C/X/V behave identically in both front ends.
    //
    // Characters and editing keys do not arrive through InputStateComponent --
    // it holds held-down key state only, with no typed-character stream and no
    // key edges. The front end therefore forwards what the window gives it via
    // PushKey/PushChar, which is the same pair of events Engine::InitInternal
    // already receives from GLFW. Events are consumed by the focused field on
    // the next frame and anything left over is dropped in EndFrame.
    bool TextInput(std::string_view label, std::string& value, const Sizing& width = {}) noexcept;

    /// Fixed-capacity overload. The field is edited through a scratch string
    /// bounded to the store's own limit, so a paste that will not fit is
    /// shortened rather than truncating the tail of the buffer.
    template <size_t N>
    bool TextInput(std::string_view label, ZHLN::FixedString<N>& value, const Sizing& width = {}) noexcept {
        std::string scratch {std::string_view(value)};
        const bool  changed = TextInputImpl(label, scratch, ZHLN::FixedString<N>::kMaxTextLength, width);
        if (changed) {
            value.assign(scratch);
        }
        return changed;
    }

    /// Forwards a key press to the focused text field. Releases are ignored:
    /// the editing rules act on presses and repeats.
    void PushKey(KeyCode key, bool pressed) noexcept;

    /// Forwards a typed character to the focused text field.
    void PushChar(unsigned int codepoint) noexcept;

    /// Where the focused field's Ctrl+C/X/V read and write. Leave unset and
    /// those three do nothing; the engine wires this to Window's clipboard.
    void SetClipboard(TextEdit::ClipboardSink sink) noexcept;

    /// True while any text field holds focus, so the caller can keep key
    /// events away from gameplay hotkeys.
    [[nodiscard]] bool IsTextInputFocused() const noexcept;

    bool BeginCollapsingHeader(std::string_view label, bool defaultOpen = false) noexcept;
    void EndCollapsingHeader() noexcept;

    template <typename Fn>
        requires std::invocable<Fn>
    void CollapsingHeader(std::string_view label, bool defaultOpen, Fn&& content) {
        if (BeginCollapsingHeader(label, defaultOpen)) {
            content();
            EndCollapsingHeader();
        }
    }

  private:
    /// The one implementation both TextInput overloads funnel into: owns focus,
    /// drains the pending key/character queue, edits `value` in place through
    /// the shared rules and draws the field. `maxTextLength` bounds what a
    /// paste may insert; std::string callers pass no limit.
    bool TextInputImpl(std::string_view label, std::string& value, size_t maxTextLength, const Sizing& width) noexcept;

    Impl* _impl = nullptr;
};

} // namespace ZHLN::GUI
