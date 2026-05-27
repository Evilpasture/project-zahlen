// File: src/engine/AssetFactory.cpp

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Font8x8.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <stb_image.h>
#include <vector>

namespace ZHLN::AssetFactory {

uint32_t CreateFontAtlasTexture(RenderContext& ctx) {
	const uint32_t atlasSize = 128;
	std::vector<uint32_t> pixels(static_cast<size_t>(atlasSize * atlasSize), 0x00000000);

	for (uint32_t c = 0; c < 128; ++c) {
		uint32_t gridX = c % 16;
		uint32_t gridY = c / 16;
		uint32_t startX = gridX * 8;
		uint32_t startY = gridY * 8;

		for (uint32_t row = 0; row < 8; ++row) {
			uint8_t byteVal = Font8x8_Basic[c][row];
			for (uint32_t col = 0; col < 8; ++col) {
				bool bit = (byteVal & (0x80 >> col)) != 0;
				uint32_t pixelX = startX + col;
				uint32_t pixelY = startY + row;

				pixels[pixelY * atlasSize + pixelX] = bit ? 0xFFFFFFFF : 0x00000000;
			}
		}
	}

	return ctx.CreateTexture(pixels.data(), atlasSize, atlasSize);
}

Mesh LoadCookedMesh(RenderContext& ctx, AssetManager& assetMgr, const std::string& virtualPath) {
#ifdef ZHLN_DEV_MODE
	std::string rawPath = "resources/assets/" + virtualPath;
	return LoadGLB(ctx, rawPath);
#else
	AssetLoadRequest req;
	req.assetID = HashAssetPath(virtualPath);

	if (!assetMgr.LoadSync(req)) {
		Log("ERROR: Failed to load cooked mesh from PAK: {}", virtualPath);
		return {};
	}

	const auto* header = static_cast<const CookedMeshHeader*>(req.outData);
	if (header->magic != 0x3048534D) {
		Log("ERROR: Invalid CookedMeshHeader magic for: {}", virtualPath);
		return {};
	}

	const auto* vertices = reinterpret_cast<const Vertex*>(header + 1);

	BufferHandle vbo = ctx.CreateVertexBuffer(vertices, header->vertexCount * sizeof(Vertex));
	assetMgr.FreeAssetMemory(req);

	Log("Loaded Cooked Mesh: {} ({} vertices)", virtualPath, header->vertexCount);
	return Mesh{.vertexBuffer = vbo, .vertexCount = header->vertexCount};
#endif
}

uint32_t LoadCookedTexture(RenderContext& ctx, AssetManager& assetMgr,
						   const std::string& virtualPath) {
#ifdef ZHLN_DEV_MODE
	std::string rawPath = "resources/assets/" + virtualPath;

	int width = 0, height = 0, channels = 0;
	unsigned char* pixels = stbi_load(rawPath.c_str(), &width, &height, &channels, 4);
	if (!pixels) {
		Log("WARNING: Failed to load raw texture in dev mode: {}", rawPath);
		return 0;
	}

	uint32_t textureIndex = ctx.CreateTexture(pixels, width, height);
	stbi_image_free(pixels);
	return textureIndex;
#else
	AssetLoadRequest req;
	req.assetID = HashAssetPath(virtualPath);

	if (!assetMgr.LoadSync(req)) {
		Log("ERROR: Failed to load texture from PAK: {}", virtualPath);
		return 0;
	}

	int width = 0;
	int height = 0;
	int channels = 0;
	unsigned char* pixels =
		stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(req.outData),
							  static_cast<int>(req.outSize), &width, &height, &channels, 4);

	if (pixels == nullptr) {
		Log("ERROR: STB failed to decode image: {}", virtualPath);
		assetMgr.FreeAssetMemory(req);
		return 0;
	}

	uint32_t textureIndex = ctx.CreateTexture(pixels, width, height);

	stbi_image_free(pixels);
	assetMgr.FreeAssetMemory(req);

	Log("Loaded Cooked Texture: {} (Bindless Index: {})", virtualPath, textureIndex);
	return textureIndex;
#endif
}

} // namespace ZHLN::AssetFactory
