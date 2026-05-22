#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <string>
#include <vector>

namespace ZHLN::GUI {

inline JPH::Mat44 CreateOrthoMatrix(float width, float height) {
	// Vulkan Y is Down: 0 screen is -1 clip (top), height screen is 1 clip (bottom).
	float r = width;
	float l = 0.0f;
	float t = 0.0f;
	float b = height;

	return JPH::Mat44(JPH::Vec4(2.0f / r, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, 2.0f / b, 0.0f, 0.0f),
					  JPH::Vec4(0.0f, 0.0f, 1.0f, 0.0f), JPH::Vec4(-1.0f, -1.0f, 0.0f, 1.0f));
}

inline Mesh CreateTextMesh(RenderContext& ctx, const std::string& text, float x, float y,
						   float scale, const JPH::Vec4& color) {
	std::vector<Vertex> vertices;
	vertices.reserve(text.length() * 6); // 6 vertices per char (2 triangles)

	float currentX = x;
	PackedRGBA8 packedColor =
		Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());
	Packed1010102 dummyNormal = Math::PackNormal(0, 1, 0);
	Packed1010102 dummyTangent = Math::PackNormal(1, 0, 0, 1);

	for (char c : text) {
		// Fallback for non-ASCII characters
		uint8_t glyphCode = static_cast<uint8_t>(c);
		if (glyphCode > 127) {
			glyphCode = '?';
		}

		// Map character grid coordinates
		uint32_t gridX = glyphCode % 16;
		uint32_t gridY = glyphCode / 16;

		// Font atlas calculation (16 columns, 16 rows, 8x8 glyphs)
		float u0 = (float)(gridX * 8) / 128.0f;
		float v0 = (float)(gridY * 8) / 128.0f;
		float u1 = (float)((gridX + 1) * 8) / 128.0f;
		float v1 = (float)((gridY + 1) * 8) / 128.0f;

		// Character quad coordinates in pixels
		float x0 = currentX;
		float y0 = y;
		float x1 = currentX + (8.0f * scale);
		float y1 = y + (8.0f * scale);

		Vertex vTL = {{x0, y0, 0.0f},		dummyNormal, dummyTangent,
					  Math::PackUV(u0, v0), packedColor, 0};
		Vertex vTR = {{x1, y0, 0.0f},		dummyNormal, dummyTangent,
					  Math::PackUV(u1, v0), packedColor, 0};
		Vertex vBL = {{x0, y1, 0.0f},		dummyNormal, dummyTangent,
					  Math::PackUV(u0, v1), packedColor, 0};
		Vertex vBR = {{x1, y1, 0.0f},		dummyNormal, dummyTangent,
					  Math::PackUV(u1, v1), packedColor, 0};

		// CCW Tri 1 (TL -> BL -> TR)
		vertices.push_back(vTL);
		vertices.push_back(vBL);
		vertices.push_back(vTR);

		// CCW Tri 2 (TR -> BL -> BR)
		vertices.push_back(vTR);
		vertices.push_back(vBL);
		vertices.push_back(vBR);

		// Move cursor for next character
		currentX += (8.0f * scale);
	}

	BufferHandle vbo = ctx.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(Vertex));
	return Mesh{.vertexBuffer = vbo, .vertexCount = static_cast<uint32_t>(vertices.size())};
}

} // namespace ZHLN::GUI
