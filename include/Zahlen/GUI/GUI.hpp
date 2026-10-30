// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Common.h>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/GUI/Font.hpp>
#include <Zahlen/GUI/TextBuffer.hpp>
#include <Zahlen/GUI/UIData.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ZHLN::GUI {

enum class Direction : uint8_t { Row, Column };
enum class Alignment : uint8_t { Start, Center, End, Stretch };

struct Sizing {
    float fixed = 0.0f;
    float grow  = 0.0f;
    bool  fit   = true;
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
    float     offsetX      = 0.0f;
    float     offsetY      = 0.0f;
    bool      clipVertical = false;
};

// Appearance of the bar drawn by Frame::BeginScrollArea. Widths and heights
// are in pixels, matching the units Clay lays out in.
struct ScrollBarStyle {
    float     width           = 6.0f;
    float     minThumb        = 20.0f;
    float     margin          = 2.0f;
    JPH::Vec4 trackColor      = {1.0f, 1.0f, 1.0f, 0.06f};
    JPH::Vec4 thumbColor      = {1.0f, 1.0f, 1.0f, 0.30f};
    JPH::Vec4 thumbHoverColor = {1.0f, 1.0f, 1.0f, 0.55f};
};

// Resolved from the previous frame's layout. scrollY is <= 0, matching the
// convention Clay uses for its scroll containers.
struct ScrollState {
    float scrollY    = 0.0f;
    float contentH   = 0.0f;
    float containerH = 0.0f;

    [[nodiscard]] auto Overflow() const noexcept -> float {
        return contentH - containerH;
    }
    [[nodiscard]] auto Scrollable() const noexcept -> bool {
        return Overflow() > 0.5f;
    }
};

struct ElementRect {
    float x      = 0.0f;
    float y      = 0.0f;
    float width  = 0.0f;
    float height = 0.0f;
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

// Per-frame input snapshot consumed by Context::BeginFrame.
// Spans are copied internally during BeginFrame and do not need to outlive the call.
struct UIInput {
    Extent2D     viewport    = {.width = 1920, .height = 1080};
    float        dt          = 1.0f / 60.0f;
    float        mouseX      = -1.0f;
    float        mouseY      = -1.0f;
    float        mouseScroll = 0.0f;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
    bool         mouseDown[3] = {false, false, false};
    KeyModifiers modifiers    = KeyModifiers::None;

    std::span<const KeyCode>  pressedKeys;
    std::span<const uint32_t> characterStream;

    TextEdit::ClipboardSink clipboard {};
};

[[nodiscard]] constexpr auto TextLineHeight(const FontAtlas& font, float scale) noexcept -> float {
    return font.LineHeight(scale);
}

[[nodiscard]] ZHLN_API auto MeasureTextBounds(const FontAtlas& font, std::string_view text, float scale) noexcept -> TextBounds;

class Frame;

class ZHLN_API Context {
  public:
    using ElementRect = GUI::ElementRect;

    Context();
    explicit Context(const FontAtlas& fontAtlas);
    ~Context() noexcept;

    Context(const Context&)                    = delete;
    auto operator=(const Context&) -> Context& = delete;
    Context(Context&&) noexcept;
    auto operator=(Context&&) noexcept -> Context&;

    void               SetFontAtlas(const FontAtlas& fontAtlas) noexcept;
    [[nodiscard]] auto GetFontAtlas() const noexcept -> const FontAtlas&;

    void SetClipboard(TextEdit::ClipboardSink sink) noexcept;

    [[nodiscard]] auto GetLastFrameRect(std::string_view id) const noexcept -> std::optional<ElementRect>;
    [[nodiscard]] auto GetScrollState(std::string_view id) const noexcept -> ScrollState;
    [[nodiscard]] auto IsPointerOver(std::string_view id) const noexcept -> bool;
    [[nodiscard]] auto IsTextInputFocused() const noexcept -> bool;
    [[nodiscard]] auto WantsCaptureMouse() const noexcept -> bool;
    [[nodiscard]] auto WantsCaptureKeyboard() const noexcept -> bool;
    [[nodiscard]] auto HasActiveFrame() const noexcept -> bool;

    // Opens a Clay layout pass for one frame and mints the ephemeral Frame capability token.
    [[nodiscard]] auto BeginFrame(const UIInput& input = {}) noexcept -> Frame;

  private:
    friend class Frame;
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

// Ephemeral capability token gating immediate-mode widget emission to an active frame.
// Minted exclusively by Context::BeginFrame; std::move(frame).End() commits draw batches,
// and ~Frame() aborts an un-ended layout pass cleanly.
class ZHLN_API Frame {
  public:
    using ElementRect = GUI::ElementRect;

    Frame(const Frame&)                        = delete;
    auto operator=(const Frame&) -> Frame&     = delete;
    Frame(Frame&& other) noexcept;
    auto operator=(Frame&&) noexcept -> Frame& = delete;
    ~Frame() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return _impl != nullptr;
    }

    [[nodiscard]] auto End() && noexcept -> UIDrawData;
    void               Cancel() noexcept;

    void BeginBox(std::string_view id, const BoxConfig& cfg = {}) noexcept;
    void EndBox() noexcept;

    void BeginRow(float gap = 0.0f, float padding = 0.0f) noexcept;
    void EndRow() noexcept;

    void BeginColumn(float gap = 0.0f, float padding = 0.0f) noexcept;
    void EndColumn() noexcept;

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

    // A vertically scrollable region. Content that overflows the box's height is
    // clipped, reachable by the mouse wheel, and reachable by dragging the bar
    // drawn flush against the box's right edge.
    //
    // `id` is mandatory: the scroll position is keyed on the element id and
    // survives across frames, so an empty id would give a container that resets
    // to the top every frame.
    void BeginScrollArea(std::string_view id, const BoxConfig& cfg = {}, const ScrollBarStyle& style = {}) noexcept;
    void EndScrollArea() noexcept;

    template <typename Fn>
        requires std::invocable<Fn>
    void ScrollArea(std::string_view id, const BoxConfig& cfg, const ScrollBarStyle& style, Fn&& content) {
        BeginScrollArea(id, cfg, style);
        content();
        EndScrollArea();
    }

    template <typename Fn>
        requires std::invocable<Fn>
    void ScrollArea(std::string_view id, const BoxConfig& cfg, Fn&& content) {
        ScrollArea(id, cfg, ScrollBarStyle {}, content);
    }

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

    auto Checkbox(std::string_view label, bool& checked, std::string_view id = {}) noexcept -> bool;
    auto Slider(std::string_view label, float& value, float minVal, float maxVal, std::string_view id = {}) noexcept -> bool;

    auto TextInput(std::string_view label, std::string& value, const Sizing& width = {}, std::string_view id = {}) noexcept -> bool;

    template <size_t N>
    auto TextInput(std::string_view label, ZHLN::FixedString<N>& value, const Sizing& width = {}, std::string_view id = {}) noexcept -> bool {
        std::string scratch {std::string_view(value)};
        const bool  changed = TextInputImpl(label, scratch, ZHLN::FixedString<N>::kMaxTextLength, width, id);
        if (changed) {
            value.assign(scratch);
        }
        return changed;
    }

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

    [[nodiscard]] auto IsItemHovered() const noexcept -> bool;
    [[nodiscard]] auto IsItemActive() const noexcept -> bool;
    [[nodiscard]] auto IsPointerPressedThisFrame() const noexcept -> bool;

    [[nodiscard]] auto Viewport() const noexcept -> Extent2D;
    [[nodiscard]] auto DeltaTime() const noexcept -> float;
    [[nodiscard]] auto GetFontAtlas() const noexcept -> const FontAtlas&;
    [[nodiscard]] auto GetLastFrameRect(std::string_view id) const noexcept -> std::optional<ElementRect>;
    [[nodiscard]] auto GetScrollState(std::string_view id) const noexcept -> ScrollState;
    [[nodiscard]] auto IsPointerOver(std::string_view id) const noexcept -> bool;
    [[nodiscard]] auto IsTextInputFocused() const noexcept -> bool;

  private:
    friend class Context;
    explicit Frame(Context::Impl* impl, void* prevClayContext) noexcept;

    auto TextInputImpl(std::string_view label, std::string& value, size_t maxTextLength, const Sizing& width, std::string_view id = {}) noexcept -> bool;

    Context::Impl* _impl            = nullptr;
    void*          _prevClayContext = nullptr;
};

} // namespace ZHLN::GUI
