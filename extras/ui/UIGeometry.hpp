// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <ui/UIComponents.hpp>
#include <Zahlen/Types.hpp>
#include <Jolt/Math/Vec4.h>
#include <cstdint>

namespace ZHLN::GUI {

auto AppendPanelVertices(
    VertexPosition* outPos, VertexAttributes* outAttr, const UIComponents::UIRectComponent& rect, const UIComponents::UIPanelComponent& panel
) -> uint32_t;

[[nodiscard]] auto CountImageVertices(const UIComponents::UIRectComponent& rect, const UIComponents::UIImageComponent& image) noexcept -> uint32_t;

auto AppendImageVertices(
    VertexPosition* outPos, VertexAttributes* outAttr, const UIComponents::UIRectComponent& rect, const UIComponents::UIImageComponent& image
) -> uint32_t;

[[nodiscard]] auto CountGradientVertices(const UIComponents::UIRectComponent& rect, const UIComponents::UIGradientComponent& gradient) noexcept
    -> uint32_t;

auto AppendGradientVertices(
    VertexPosition* outPos, VertexAttributes* outAttr, const UIComponents::UIRectComponent& rect, const UIComponents::UIGradientComponent& gradient
) -> uint32_t;

[[nodiscard]] auto CountPlotVertices(const UIComponents::UIPlotComponent& plot) noexcept -> uint32_t;

auto AppendPlotVertices(
    VertexPosition* outPos, VertexAttributes* outAttr, const UIComponents::UIRectComponent& rect, const UIComponents::UIPlotComponent& plot
) -> uint32_t;

[[nodiscard]] auto HsvToRgb(float hue, float sat, float val) noexcept -> JPH::Vec4;
void               RgbToHsv(const JPH::Vec4& rgb, float& hueInOut, float& satOut, float& valOut) noexcept;

} // namespace ZHLN::GUI
