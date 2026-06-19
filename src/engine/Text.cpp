// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Zahlen/Components.hpp"

#include <Zahlen/GUI.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <string>
#include <vector>

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
	std::vector<Vertex> vertices;
	vertices.reserve(text.length() * 6);

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

		Vertex vTL = {
			.position = {x0, y0, 0.0f},
			.normal = dummyNormal,
			.tangent = dummyTangent,
			.uv = Math::PackUV(u0, v0),
			.color = packedColor,
			.joints = {},
			.weights = {},
			._padding = {},
		};
		Vertex vTR = {
			.position = {x1, y0, 0.0f},
			.normal = dummyNormal,
			.tangent = dummyTangent,
			.uv = Math::PackUV(u1, v0),
			.color = packedColor,
			.joints = {},
			.weights = {},
			._padding = {},
		};
		Vertex vBL = {
			.position = {x0, y1, 0.0f},
			.normal = dummyNormal,
			.tangent = dummyTangent,
			.uv = Math::PackUV(u0, v1),
			.color = packedColor,
			.joints = {},
			.weights = {},
			._padding = {},
		};
		Vertex vBR = {
			.position = {x1, y1, 0.0f},
			.normal = dummyNormal,
			.tangent = dummyTangent,
			.uv = Math::PackUV(u1, v1),
			.color = packedColor,
			.joints = {},
			.weights = {},
			._padding = {},
		};

		vertices.push_back(vTL);
		vertices.push_back(vBL);
		vertices.push_back(vTR);

		vertices.push_back(vTR);
		vertices.push_back(vBL);
		vertices.push_back(vBR);

		// Advance cursor using proportional TrueType spacing
		currentX += g.xadvance * scale;
	}

	BufferHandle vbo = ctx.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(Vertex));
	return Mesh{.vertexBuffer = vbo, .vertexCount = static_cast<uint32_t>(vertices.size())};
}

} // namespace ZHLN::GUI
