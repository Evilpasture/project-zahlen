#include "Resources.hpp"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Math3D.hpp> // For PackNormal, PackColor, etc.
#include <vector>

namespace ZHLN::AssetFactory {

Mesh CreatePlane(RenderContext& ctx, float extent, const JPH::Vec4& color) {
	Packed1010102 n = Math::PackNormal(0.0f, 1.0f, 0.0f);
	Packed1010102 t = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f);
	PackedRGBA8 c = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

	std::vector<Vertex> data = {
		{{-extent, 0.0f, extent}, n, t, Math::PackUV(0.0f, 1.0f), c, 0},
		{{extent, 0.0f, extent}, n, t, Math::PackUV(1.0f, 1.0f), c, 0},
		{{extent, 0.0f, -extent}, n, t, Math::PackUV(1.0f, 0.0f), c, 0},
		{{extent, 0.0f, -extent}, n, t, Math::PackUV(1.0f, 0.0f), c, 0},
		{{-extent, 0.0f, -extent}, n, t, Math::PackUV(0.0f, 0.0f), c, 0},
		{{-extent, 0.0f, extent}, n, t, Math::PackUV(0.0f, 1.0f), c, 0},
	};

	BufferHandle vbo = ctx.CreateVertexBuffer(data.data(), data.size() * sizeof(Vertex));
	return Mesh{.vertexBuffer = vbo, .vertexCount = static_cast<uint32_t>(data.size())};
}

Mesh CreateBox(RenderContext& ctx, JPH::Vec3Arg halfExtents, const JPH::Vec4& color) {
	const float x = halfExtents.GetX(), y = halfExtents.GetY(), z = halfExtents.GetZ();
	PackedRGBA8 c = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

	// Front/Back/Top/Bottom/Right/Left normals
	Packed1010102 nZ = Math::PackNormal(0, 0, 1), tZ = Math::PackNormal(1, 0, 0, 1);
	Packed1010102 nNZ = Math::PackNormal(0, 0, -1), tNZ = Math::PackNormal(-1, 0, 0, 1);
	Packed1010102 nY = Math::PackNormal(0, 1, 0), tY = Math::PackNormal(1, 0, 0, 1);
	Packed1010102 nNY = Math::PackNormal(0, -1, 0), tNY = Math::PackNormal(1, 0, 0, 1);
	Packed1010102 nX = Math::PackNormal(1, 0, 0), tX = Math::PackNormal(0, 0, -1, 1);
	Packed1010102 nNX = Math::PackNormal(-1, 0, 0), tNX = Math::PackNormal(0, 0, 1, 1);

	auto uv00 = Math::PackUV(0.0f, 0.0f), uv10 = Math::PackUV(1.0f, 0.0f);
	auto uv01 = Math::PackUV(0.0f, 1.0f), uv11 = Math::PackUV(1.0f, 1.0f);

	std::vector<Vertex> data = {// Front (+Z)
								{{-x, -y, z}, nZ, tZ, uv01, c, 0},
								{{x, -y, z}, nZ, tZ, uv11, c, 0},
								{{x, y, z}, nZ, tZ, uv10, c, 0},
								{{x, y, z}, nZ, tZ, uv10, c, 0},
								{{-x, y, z}, nZ, tZ, uv00, c, 0},
								{{-x, -y, z}, nZ, tZ, uv01, c, 0},
								// Back (-Z)
								{{x, -y, -z}, nNZ, tNZ, uv01, c, 0},
								{{-x, -y, -z}, nNZ, tNZ, uv11, c, 0},
								{{-x, y, -z}, nNZ, tNZ, uv10, c, 0},
								{{-x, y, -z}, nNZ, tNZ, uv10, c, 0},
								{{x, y, -z}, nNZ, tNZ, uv00, c, 0},
								{{x, -y, -z}, nNZ, tNZ, uv01, c, 0},
								// Top (+Y)
								{{-x, y, z}, nY, tY, uv01, c, 0},
								{{x, y, z}, nY, tY, uv11, c, 0},
								{{x, y, -z}, nY, tY, uv10, c, 0},
								{{x, y, -z}, nY, tY, uv10, c, 0},
								{{-x, y, -z}, nY, tY, uv00, c, 0},
								{{-x, y, z}, nY, tY, uv01, c, 0},
								// Bottom (-Y)
								{{-x, -y, -z}, nNY, tNY, uv01, c, 0},
								{{x, -y, -z}, nNY, tNY, uv11, c, 0},
								{{x, -y, z}, nNY, tNY, uv10, c, 0},
								{{x, -y, z}, nNY, tNY, uv10, c, 0},
								{{-x, -y, z}, nNY, tNY, uv00, c, 0},
								{{-x, -y, -z}, nNY, tNY, uv01, c, 0},
								// Right (+X)
								{{x, -y, z}, nX, tX, uv01, c, 0},
								{{x, -y, -z}, nX, tX, uv11, c, 0},
								{{x, y, -z}, nX, tX, uv10, c, 0},
								{{x, y, -z}, nX, tX, uv10, c, 0},
								{{x, y, z}, nX, tX, uv00, c, 0},
								{{x, -y, z}, nX, tX, uv01, c, 0},
								// Left (-X)
								{{-x, -y, -z}, nNX, tNX, uv01, c, 0},
								{{-x, -y, z}, nNX, tNX, uv11, c, 0},
								{{-x, y, z}, nNX, tNX, uv10, c, 0},
								{{-x, y, z}, nNX, tNX, uv10, c, 0},
								{{-x, y, -z}, nNX, tNX, uv00, c, 0},
								{{-x, -y, -z}, nNX, tNX, uv01, c, 0}};

	BufferHandle vbo = ctx.CreateVertexBuffer(data.data(), data.size() * sizeof(Vertex));
	return Mesh{.vertexBuffer = vbo, .vertexCount = static_cast<uint32_t>(data.size())};
}

auto CreateProceduralCheckerboard(RenderContext& ctx) {
	const uint32_t size = 256;
	std::vector<uint32_t> pixels(size * size);
	for (uint32_t y = 0; y < size; y++) {
		for (uint32_t x = 0; x < size; x++) {
			// XOR logic creates a perfect grid
			bool isWhite = ((x / 32) + (y / 32)) % 2 == 0;
			pixels[y * size + x] = isWhite ? 0xFFFFFFFF : 0xFF333333;
		}
	}
	// Upload this to your Bindless Registry
	return ctx.CreateTexture(pixels.data(), size, size);
}

Material CreateBasicMaterial(RenderContext& ctx) {
	PipelineDesc desc;
	desc.vertexShaderData = ZHLN_Resource_BasicVertSpv;
	desc.vertexShaderSize = ZHLN_Resource_BasicVertSpv_Len;
	desc.fragShaderData = ZHLN_Resource_BasicFragSpv;
	desc.fragShaderSize = ZHLN_Resource_BasicFragSpv_Len;

	Material mat = ctx.CreateMaterial(desc);

	// Generate the procedural texture and save its index!
	mat.textureIndex = CreateProceduralCheckerboard(ctx);

	return mat;
}

} // namespace ZHLN::AssetFactory