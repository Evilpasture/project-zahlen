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
}

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
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    bool clipVertical = false;
};

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

    void BeginFrame(float dt) noexcept;
    [[nodiscard]] UIDrawData EndFrame() noexcept;

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

    [[nodiscard]] auto IsItemHovered() const noexcept -> bool;

    struct ElementRect {
        float x      = 0.0f;
        float y      = 0.0f;
        float width  = 0.0f;
        float height = 0.0f;
    };
    [[nodiscard]] auto GetLastFrameRect(std::string_view id) const noexcept -> std::optional<ElementRect>;
    [[nodiscard]] auto IsItemActive() const noexcept -> bool;

    [[nodiscard]] auto IsPointerOver(std::string_view id) const noexcept -> bool;

    [[nodiscard]] auto IsPointerPressedThisFrame() const noexcept -> bool;

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

    void PushKey(KeyCode key, bool pressed) noexcept;

    void PushChar(unsigned int codepoint) noexcept;

    void SetClipboard(TextEdit::ClipboardSink sink) noexcept;

    [[nodiscard]] auto IsTextInputFocused() const noexcept -> bool;

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
    auto TextInputImpl(std::string_view label, std::string& value, size_t maxTextLength, const Sizing& width, std::string_view id = {}) noexcept -> bool;

    Impl* _impl = nullptr;
};

}
