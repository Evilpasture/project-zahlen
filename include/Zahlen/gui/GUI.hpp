// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Common.h>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/gui/TextBuffer.hpp>
#include <Zahlen/Geometry2D.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/gui/Font.hpp>
#include <Zahlen/gui/UIData.hpp>
#include <concepts>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ZHLN {
class Engine;
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
    // Pixel offset from the parent's top-left. Non-zero takes the box out of
    // flex flow (Clay floating attach). Zero keeps ordinary layout.
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    // Clip overflowing children on Y and let the mouse wheel scroll them.
    // The box must have an id so Clay can keep the offset across frames.
    bool clipVertical = false;
};

// Scene singleton that owns the baked font atlas.
//
// Immediate-mode Clay (`GUI::Context`) reads `fontAtlas` each BeginFrame.
// Lives on the registry rather than on Context because Context is rebuilt
// every frame. The atlas arrives pre-baked (see <Zahlen/gui/FontLoader.hpp>);
// core never rasterises a font at runtime.
struct UISettingsComponent {
    TextureHandle defaultFontAtlas = TextureHandle::Invalid;
    FontAtlas     fontAtlas;
};

struct TextBounds {
    float              minX = 0.0f;
    float              maxX = 0.0f;
    float              minY = 0.0f;
    float              maxY = 0.0f;
    [[nodiscard]] auto width() const noexcept -> float {
        return maxX - minX;
    }
    [[nodiscard]] auto height() const noexcept -> float {
        return maxY - minY;
    }
};

/// Line advance of @p font laid out at @p scale (FontAtlas::ScaleFor's return).
/// The number is the bake's own lineHeight scaled to the requested pixel size;
/// nothing here assumes any particular font size.
[[nodiscard]] constexpr auto TextLineHeight(const FontAtlas& font, float scale) noexcept -> float {
    return font.LineHeight(scale);
}

[[nodiscard]] auto MeasureTextBounds(const FontAtlas& font, std::string_view text, float scale) noexcept -> TextBounds;

class ZHLN_API Context {
  public:
    struct Impl;

    explicit Context(Engine& engine) noexcept;
    explicit Context(ECS::Registry& registry, Extent2D viewport = {.width = 1920, .height = 1080}) noexcept;
    ~Context() noexcept = default;

    Context(const Context&)                        = default;
    auto operator=(const Context&) -> Context&     = default;
    Context(Context&&) noexcept                    = default;
    auto operator=(Context&&) noexcept -> Context& = default;

    // --- Frame Lifecycle
    // EndFrame closes the layout and extracts the frame's draw data: plain geometry handed to
    // the renderer through `RenderContext::RenderUI`, so the GUI never inherits a renderer
    // interface or knows a renderer. The returned spans alias storage this context owns until
    // its next BeginFrame -- consume them in the same frame.
    void BeginFrame(float dt) noexcept;
    [[nodiscard]] UIDrawData EndFrame() noexcept;

    // --- Layout Containers (Macro-free C++ API)
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

    // --- Interactive Widgets
    void Text(std::string_view text, float fontSize = 16.0f, const JPH::Vec4& color = {1, 1, 1, 1}) noexcept;
    auto Button(std::string_view label, const JPH::Vec4& color = {0.16f, 0.24f, 0.36f, 0.95f}, const Sizing& width = {}, std::string_view id = {}) noexcept
        -> bool;
    auto Button(std::string_view label, const Sizing& width) noexcept -> bool {
        return Button(label, {0.16f, 0.24f, 0.36f, 0.95f}, width);
    }

    template <typename OnClickFn>
        requires std::invocable<OnClickFn>
    auto Button(std::string_view label, OnClickFn&& onClick) -> bool {
        if (Button(label)) {
            onClick();
            return true;
        }
        return false;
    }

    template <typename OnClickFn, typename OnHoverFn>
        requires std::invocable<OnClickFn> && std::invocable<OnHoverFn>
    auto Button(std::string_view label, OnClickFn&& onClick, OnHoverFn&& onHover) -> bool {
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
    auto Button(std::string_view label, const Sizing& width, OnClickFn&& onClick) -> bool {
        if (Button(label, width)) {
            onClick();
            return true;
        }
        return false;
    }

    template <typename OnClickFn, typename OnHoverFn>
        requires std::invocable<OnClickFn> && std::invocable<OnHoverFn>
    auto Button(std::string_view label, const Sizing& width, OnClickFn&& onClick, OnHoverFn&& onHover) -> bool {
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
    auto Button(std::string_view label, const JPH::Vec4& color, const Sizing& width, OnClickFn&& onClick) -> bool {
        if (Button(label, color, width)) {
            onClick();
            return true;
        }
        return false;
    }

    template <typename OnClickFn, typename OnHoverFn>
        requires std::invocable<OnClickFn> && std::invocable<OnHoverFn>
    auto Button(std::string_view label, const JPH::Vec4& color, const Sizing& width, OnClickFn&& onClick, OnHoverFn&& onHover) -> bool {
        bool clicked = Button(label, color, width);
        if (IsItemHovered()) {
            onHover();
        }
        if (clicked) {
            onClick();
        }
        return clicked;
    }

    // --- State Inspection
    [[nodiscard]] auto IsItemHovered() const noexcept -> bool;

    // Rectangle, in window pixels with a top-left origin, that the element
    // registered under @p id (the same string handed to Box/Button) occupied
    // in LAST frame's layout. Returns nothing until the element has been laid
    // out at least once -- callers keep their fallback for the first frame.
    struct ElementRect {
        float x      = 0.0f;
        float y      = 0.0f;
        float width  = 0.0f;
        float height = 0.0f;
    };
    [[nodiscard]] auto GetLastFrameRect(std::string_view id) const noexcept -> std::optional<ElementRect>;
    [[nodiscard]] auto IsItemActive() const noexcept -> bool;

    // True if the pointer sits inside the last-frame rectangle of @p id
    // (the same string handed to Box). First frame is always false.
    [[nodiscard]] auto IsPointerOver(std::string_view id) const noexcept -> bool;

    // True on the frame the pointer went down, independent of any widget.
    [[nodiscard]] auto IsPointerPressedThisFrame() const noexcept -> bool;

    auto Checkbox(std::string_view label, bool& checked, std::string_view id = {}) noexcept -> bool;
    auto Slider(std::string_view label, float& value, float minVal, float maxVal, std::string_view id = {}) noexcept -> bool;

    // --- Text Input
    // Single-line editable field, returning true on any frame the text changed. Caret
    // movement, selection, word deletion and Ctrl+C/X/V come from Zahlen/gui/TextBuffer.hpp, so
    // they are unit-testable without a display.
    //
    // Characters and editing keys do not arrive through InputStateComponent -- it holds
    // held-down key state only, with no typed-character stream and no key edges -- so the front
    // end forwards what the window gives it via PushKey/PushChar (the same events
    // Engine::InitInternal already receives from GLFW). Events are consumed by the focused
    // field on the next frame; the rest is dropped in EndFrame.
    auto TextInput(std::string_view label, std::string& value, const Sizing& width = {}, std::string_view id = {}) noexcept -> bool;

    // Fixed-capacity overload. The field is edited through a scratch string
    // bounded to the store's own limit, so a paste that will not fit is
    // shortened rather than truncating the tail of the buffer.
    template <size_t N>
    auto TextInput(std::string_view label, ZHLN::FixedString<N>& value, const Sizing& width = {}, std::string_view id = {}) noexcept -> bool {
        std::string scratch {std::string_view(value)};
        const bool  changed = TextInputImpl(label, scratch, ZHLN::FixedString<N>::kMaxTextLength, width, id);
        if (changed) {
            value.assign(scratch);
        }
        return changed;
    }

    // Forwards a key press to the focused text field. Releases are ignored:
    // the editing rules act on presses and repeats.
    void PushKey(KeyCode key, bool pressed) noexcept;

    // Forwards a typed character to the focused text field.
    void PushChar(unsigned int codepoint) noexcept;

    // Where the focused field's Ctrl+C/X/V read and write. Leave unset and
    // those three do nothing; the engine wires this to Window's clipboard.
    void SetClipboard(TextEdit::ClipboardSink sink) noexcept;

    // True while any text field holds focus, so the caller can keep key
    // events away from gameplay hotkeys.
    [[nodiscard]] auto IsTextInputFocused() const noexcept -> bool;

    // --- Dropdown
    // Single-selection list: `options` are the labels, `selected` an index into them (clamped,
    // never written out of range), and the return is true on the frame the selection changed.
    // The list is a Clay floating element anchored under the field, so opening it does not push
    // the panel down; clicking the field toggles, an option selects and closes, anywhere else
    // closes, and while open Up/Down move the highlight with Enter/Escape closing through the
    // same key path TextInput uses.
    auto Dropdown(std::string_view label, std::span<const std::string_view> options, int& selected, const Sizing& width = {}) noexcept -> bool;

    auto BeginCollapsingHeader(std::string_view label, bool defaultOpen = false) noexcept -> bool;
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
    // The one implementation both TextInput overloads funnel into: owns focus,
    // drains the pending key/character queue, edits `value` in place through
    // the shared rules and draws the field. `maxTextLength` bounds what a
    // paste may insert; std::string callers pass no limit.
    auto TextInputImpl(std::string_view label, std::string& value, size_t maxTextLength, const Sizing& width, std::string_view id = {}) noexcept -> bool;

    Impl* _impl = nullptr;
};

} // namespace ZHLN::GUI
