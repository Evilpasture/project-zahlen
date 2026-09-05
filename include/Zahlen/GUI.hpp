// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Common.h>
#include <Zahlen/Types.hpp>
#include <concepts>
#include <memory>
#include <string_view>

namespace ZHLN {
class Engine;
class RenderContext;
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
    explicit Context(Engine& engine) noexcept;
    ~Context() noexcept;

    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) noexcept;
    Context& operator=(Context&&) noexcept;

    // --- Frame Lifecycle ---
    void BeginFrame(float dt) noexcept;
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
    void Text(std::string_view text, float fontSize = 14.0f, const JPH::Vec4& color = {1, 1, 1, 1}) noexcept;
    bool Button(std::string_view label, const JPH::Vec4& color = {0.16f, 0.24f, 0.36f, 0.95f}, const Sizing& width = {}) noexcept;
    bool Button(std::string_view label, const Sizing& width) noexcept {
        return Button(label, {0.16f, 0.24f, 0.36f, 0.95f}, width);
    }
    bool Checkbox(std::string_view label, bool& checked) noexcept;
    bool Slider(std::string_view label, float& value, float minVal, float maxVal) noexcept;

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
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN::GUI
