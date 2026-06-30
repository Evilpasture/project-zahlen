// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Components.hpp"
#include "Render.hpp"
#include "Types.hpp"

#include <string>

namespace ZHLN::GUI {
Mesh CreateTextMesh(RenderContext& ctx, const FontAtlas& font, const std::string& text, float x,
					float y, float scale, const JPH::Vec4& color);
Mesh CreatePanelMesh(RenderContext& ctx, const UIRectComponent& rect,
					 const UIPanelComponent& panel);
} // namespace ZHLN::GUI
