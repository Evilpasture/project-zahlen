// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/AssetFactory.cpp

#include "Zahlen/AssetManager.hpp"
#include "Zahlen/Render.hpp"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Font8x8.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <cstddef>
#include <stb_image.h>

namespace ZHLN::AssetFactory {

uint32_t CreateFontAtlasTexture(RenderContext& ctx) {
	const uint32_t atlasSize = 128;
	// Minimal fallback array (no vector)
	auto* pixels = new uint32_t[static_cast<size_t>(atlasSize * atlasSize)];

	for (uint32_t i = 0; i < atlasSize * atlasSize; ++i) {
		pixels[i] = 0x00000000;
	}

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

	uint32_t tex = ctx.CreateTexture(pixels, atlasSize, atlasSize);
	delete[] pixels;
	return tex;
}

Mesh LoadCookedMesh(RenderContext& ctx, [[maybe_unused]] AssetManager& assetMgr,
					std::string_view virtualPath) {
	if constexpr (isDev) {
		auto* prefab = LoadModelPrefab(ctx, assetMgr, virtualPath);
		if ((prefab != nullptr) && prefab->partCount > 0) {
			return prefab->parts[0].mesh;
		}
		return {};
	} else {
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

		// 1. Resolve VBO
		const auto* vertices = std::bit_cast<const Vertex*>(header + 1);
		BufferHandle vbo = ctx.CreateVertexBuffer(vertices, header->vertexCount * sizeof(Vertex));

		// 2. Resolve IBO (index array follows vertex array in binary)
		BufferHandle ibo = BufferHandle::Invalid;
		if (header->indexCount > 0) {
			const auto* indices = std::bit_cast<const uint32_t*>(vertices + header->vertexCount);
			ibo = ctx.CreateIndexBuffer(indices, header->indexCount * sizeof(uint32_t));
		}

		assetMgr.FreeAssetMemory(req);

		Log("Loaded Cooked Indexed Mesh: {} ({} vertices, {} indices)", virtualPath,
			header->vertexCount, header->indexCount);

		return Mesh{.vertexBuffer = vbo,
					.indexBuffer = ibo,
					.vertexCount = header->vertexCount,
					.indexCount = header->indexCount};
	}
}

uint32_t LoadCookedTexture(RenderContext& ctx, [[maybe_unused]] AssetManager& assetMgr,
						   std::string_view virtualPath) {
	if constexpr (isDev) {
		std::string rawPath = "resources/assets/" + std::string(virtualPath);

		int width = 0;
		int height = 0;
		int channels = 0;
		unsigned char* pixels = stbi_load(rawPath.c_str(), &width, &height, &channels, 4);
		if (pixels == nullptr) {
			Log("WARNING: Failed to load raw texture in dev mode: {}", rawPath);
			return 0;
		}

		uint32_t textureIndex = ctx.CreateTexture(pixels, width, height);
		stbi_image_free(pixels);
		return textureIndex;
	} else {
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
			stbi_load_from_memory(std::bit_cast<const stbi_uc*>(req.outData),
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
	}
}

} // namespace ZHLN::AssetFactory
