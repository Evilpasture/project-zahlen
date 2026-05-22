#pragma once
#include "Render.hpp"
#include "Types.hpp"

#include <string>

namespace ZHLN::GUI {
JPH::Mat44 CreateOrthoMatrix(float width, float height);
Mesh CreateTextMesh(RenderContext& ctx, const std::string& text, float x, float y, float scale,
					const JPH::Vec4& color);
} // namespace ZHLN::GUI
