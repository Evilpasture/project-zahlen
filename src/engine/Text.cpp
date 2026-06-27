// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/Text.cpp
#include "Zahlen/Components.hpp"

#include <Zahlen/GUI.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <string>

namespace ZHLN::GUI {

auto CreateOrthoMatrix(float width, float height) -> JPH::Mat44 {
	float r = width;
	float b = height;

	// Invert the Y-scale (-2.0f / b) and Y-translation (+1.0f)
	// to perfectly compensate for Vulkan's negative-height viewport.
	return {JPH::Vec4(2.0f / r, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, -2.0f / b, 0.0f, 0.0f),
			JPH::Vec4(0.0f, 0.0f, 1.0f, 0.0f), JPH::Vec4(-1.0f, 1.0f, 0.0f, 1.0f)};
}

auto CreateTextMesh(RenderContext& ctx, const FontAtlas& font, const std::string& text, float x,
					float y, float scale, const JPH::Vec4& color) -> Mesh {
	if (text.empty()) {
		return {};
	}

	JPH::Array<VertexPosition> positions;
	JPH::Array<VertexAttributes> attributes;
	positions.reserve(text.length() * 6);
	attributes.reserve(text.length() * 6);

	float currentX = x;
	PackedRGBA8 packedColor =
		Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());
	Packed1010102 dummyNormal = Math::PackNormal(0, 1, 0);
	Packed1010102 dummyTangent = Math::PackNormal(1, 0, 0, 1);

	for (char c : text) {
		uint32_t glyphCode = static_cast<uint8_t>(c);
		if (glyphCode < 32 || glyphCode > 127) {
			glyphCode = '?';
		}

		// Retrieve layout bounds directly from the system font atlas
		const auto& g = font.glyphs[glyphCode - 32];

		// Normalize coordinate bounds to [0.0 - 1.0] inside the 512x512 texture
		float u0 = g.x0 / 512.0f;
		float v0 = g.y0 / 512.0f;
		float u1 = g.x1 / 512.0f;
		float v1 = g.y1 / 512.0f;

		// Align bounding box using TrueType kerning offsets
		float x0 = currentX + g.xoff * scale;
		float y0 = y + g.yoff * scale;
		float x1 = x0 + (g.x1 - g.x0) * scale;
		float y1 = y0 + (g.y1 - g.y0) * scale;

		VertexPosition vTL_pos = {{x0, y0, 0.0f}};
		VertexAttributes vTL_attr = {.normal = dummyNormal,
									 .tangent = dummyTangent,
									 .uv = Math::PackUV(u0, v0),
									 .color = packedColor};

		VertexPosition vTR_pos = {{x1, y0, 0.0f}};
		VertexAttributes vTR_attr = {.normal = dummyNormal,
									 .tangent = dummyTangent,
									 .uv = Math::PackUV(u1, v0),
									 .color = packedColor};

		VertexPosition vBL_pos = {{x0, y1, 0.0f}};
		VertexAttributes vBL_attr = {.normal = dummyNormal,
									 .tangent = dummyTangent,
									 .uv = Math::PackUV(u0, v1),
									 .color = packedColor};

		VertexPosition vBR_pos = {{x1, y1, 0.0f}};
		VertexAttributes vBR_attr = {.normal = dummyNormal,
									 .tangent = dummyTangent,
									 .uv = Math::PackUV(u1, v1),
									 .color = packedColor};

		// Triangle 1: TL -> BL -> TR
		positions.push_back(vTL_pos);
		attributes.push_back(vTL_attr);

		positions.push_back(vBL_pos);
		attributes.push_back(vBL_attr);

		positions.push_back(vTR_pos);
		attributes.push_back(vTR_attr);

		// Triangle 2: TR -> BL -> BR
		positions.push_back(vTR_pos);
		attributes.push_back(vTR_attr);

		positions.push_back(vBL_pos);
		attributes.push_back(vBL_attr);

		positions.push_back(vBR_pos);
		attributes.push_back(vBR_attr);

		// Advance cursor using proportional TrueType spacing
		currentX += g.xadvance * scale;
	}

	BufferHandle posVbo = ctx.CreateVertexBuffer(
		positions.data(), positions.size() * sizeof(VertexPosition), sizeof(VertexPosition));
	BufferHandle attrVbo = ctx.CreateVertexBuffer(
		attributes.data(), attributes.size() * sizeof(VertexAttributes), sizeof(VertexAttributes));

	return Mesh{.posBuffer = posVbo,
				.attrBuffer = attrVbo,
				.skinBuffer = BufferHandle::Invalid,
				.indexBuffer = BufferHandle::Invalid,
				.vertexCount = static_cast<uint32_t>(positions.size()),
				.indexCount = 0};
}

auto CreatePanelMesh(RenderContext& ctx, const UIRectComponent& rect, const UIPanelComponent& panel)
	-> Mesh {

	float x0 = rect.computedAbsMinX;
	float y0 = rect.computedAbsMinY;
	float x1 = rect.computedAbsMaxX;
	float y1 = rect.computedAbsMaxY;

	PackedRGBA8 c = Math::PackColor(panel.color.GetX(), panel.color.GetY(), panel.color.GetZ(),
									panel.color.GetW());
	Packed1010102 n = Math::PackNormal(0, 0, 1);
	Packed1010102 t = Math::PackNormal(1, 0, 0, 1);

	JPH::Array<VertexPosition> positions;
	JPH::Array<VertexAttributes> attributes;

	// Check if 9-slice is enabled and bounds are sufficient
	float width = x1 - x0;
	float height = y1 - y0;

	if (panel.edgeWidth > 0.0f && width > 0.0f && height > 0.0f) {
		// Clamp edge sizes to prevent overlap if the panel is smaller than the borders
		float borderX = std::min(panel.edgeWidth, width * 0.5f);
		float borderY = std::min(panel.edgeWidth, height * 0.5f);

		// Coordinate grids (screen-space and texture-space)
		float xs[4] = {x0, x0 + borderX, x1 - borderX, x1};
		float ys[4] = {y0, y0 + borderY, y1 - borderY, y1};
		float us[4] = {0.0f, panel.uvLeft, 1.0f - panel.uvRight, 1.0f};
		float vs[4] = {0.0f, panel.uvTop, 1.0f - panel.uvBottom, 1.0f};

		positions.reserve(54); // 9 quads * 6 vertices
		attributes.reserve(54);

		// Construct 9 quads without indices (flat stream)
		for (int r = 0; r < 3; ++r) {
			for (int col = 0; col < 3; ++col) {
				float qx0 = xs[col];
				float qx1 = xs[col + 1];
				float qy0 = ys[r];
				float qy1 = ys[r + 1];

				float qu0 = us[col];
				float qu1 = us[col + 1];
				float qv0 = vs[r];
				float qv1 = vs[r + 1];

				// Triangle 1: Top-Left -> Bottom-Left -> Top-Right
				positions.push_back({{qx0, qy0, 0.0f}});
				attributes.push_back(
					{.normal = n, .tangent = t, .uv = Math::PackUV(qu0, qv0), .color = c});

				positions.push_back({{qx0, qy1, 0.0f}});
				attributes.push_back(
					{.normal = n, .tangent = t, .uv = Math::PackUV(qu0, qv1), .color = c});

				positions.push_back({{qx1, qy0, 0.0f}});
				attributes.push_back(
					{.normal = n, .tangent = t, .uv = Math::PackUV(qu1, qv0), .color = c});

				// Triangle 2: Top-Right -> Bottom-Left -> Bottom-Right
				positions.push_back({{qx1, qy0, 0.0f}});
				attributes.push_back(
					{.normal = n, .tangent = t, .uv = Math::PackUV(qu1, qv0), .color = c});

				positions.push_back({{qx0, qy1, 0.0f}});
				attributes.push_back(
					{.normal = n, .tangent = t, .uv = Math::PackUV(qu0, qv1), .color = c});

				positions.push_back({{qx1, qy1, 0.0f}});
				attributes.push_back(
					{.normal = n, .tangent = t, .uv = Math::PackUV(qu1, qv1), .color = c});
			}
		}
	} else {
		// Fallback: standard stretched quad (6 vertices)
		positions.resize(6);
		attributes.resize(6);

		positions[0] = {{x0, y0, 0.0f}};
		attributes[0] = {
			.normal = n, .tangent = t, .uv = Math::PackUV(0.0f, 0.0f), .color = c}; // TL
		positions[1] = {{x0, y1, 0.0f}};
		attributes[1] = {
			.normal = n, .tangent = t, .uv = Math::PackUV(0.0f, 1.0f), .color = c}; // BL
		positions[2] = {{x1, y0, 0.0f}};
		attributes[2] = {
			.normal = n, .tangent = t, .uv = Math::PackUV(1.0f, 0.0f), .color = c}; // TR

		positions[3] = {{x1, y0, 0.0f}};
		attributes[3] = {
			.normal = n, .tangent = t, .uv = Math::PackUV(1.0f, 0.0f), .color = c}; // TR
		positions[4] = {{x0, y1, 0.0f}};
		attributes[4] = {
			.normal = n, .tangent = t, .uv = Math::PackUV(0.0f, 1.0f), .color = c}; // BL
		positions[5] = {{x1, y1, 0.0f}};
		attributes[5] = {
			.normal = n, .tangent = t, .uv = Math::PackUV(1.0f, 1.0f), .color = c}; // BR
	}

	BufferHandle posVbo = ctx.CreateVertexBuffer(
		positions.data(), positions.size() * sizeof(VertexPosition), sizeof(VertexPosition));
	BufferHandle attrVbo = ctx.CreateVertexBuffer(
		attributes.data(), attributes.size() * sizeof(VertexAttributes), sizeof(VertexAttributes));

	return Mesh{.posBuffer = posVbo,
				.attrBuffer = attrVbo,
				.vertexCount = static_cast<uint32_t>(positions.size()),
				.indexCount = 0};
}

} // namespace ZHLN::GUI
