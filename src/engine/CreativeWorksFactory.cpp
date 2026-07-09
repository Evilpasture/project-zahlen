// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/CreativeWorksFactory.cpp

#include "Zahlen/Components.hpp"
#include "Zahlen/CreativeWorksManager.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Render.hpp"
#include "ecs/ECS.hpp"

#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Font8x8.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <algorithm>
#include <cstddef>
#include <fontconfig/fontconfig.h>
#include <stb_image.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace ZHLN::CreativeWorksFactory {

static std::string FindSystemFont(const char* fontName) {
	FcConfig* config = FcInitLoadConfigAndFonts();
	FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(fontName));
	FcConfigSubstitute(config, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);

	FcResult result;
	FcPattern* match = FcFontMatch(config, pat, &result);
	std::string fontPath;
	if (match != nullptr) {
		FcChar8* file = nullptr;
		if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
			fontPath = reinterpret_cast<const char*>(file);
		}
		FcPatternDestroy(match);
	}
	FcPatternDestroy(pat);
	FcConfigDestroy(config);
	return fontPath;
}

uint32_t CreateFontAtlasTexture(RenderContext& ctx) {
	// Query standard Arch sans-serif font (fallback to DejaVu/Liberation if missing)
	std::string fontPath = FindSystemFont("sans-serif");
	if (fontPath.empty()) {
		fontPath = "/usr/share/fonts/TTF/DejaVuSans.ttf"; // Safe Arch fallback
	}

	Log("Loading TrueType system font: {}", fontPath);

	FILE* f = std::fopen(fontPath.c_str(), "rb");
	if (f == nullptr) {
		Log("ERROR: Failed to open system font file: {}", fontPath);
		return 0;
	}

	std::fseek(f, 0, SEEK_END);
	long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> fontBuffer(size);
	std::fread(fontBuffer.data(), 1, size, f);
	std::fclose(f);

	// Resolve starting offset for font collections (like macOS .ttc files)
	int fontOffset = stbtt_GetFontOffsetForIndex(fontBuffer.data(), 0);
	fontOffset = std::max(fontOffset, 0); // Fallback to standard behavior for standard TTF

	// Setup a clean 512x512 alpha map
	const uint32_t atlasSize = 512;
	std::vector<uint8_t> alphaBitmap(static_cast<size_t>(atlasSize * atlasSize), 0);

	// Retrieve active font settings from your Components::UISettingsComponent
	auto* engine = GetEngineContext();
	auto& reg = engine->GetRegistry();

	auto uiSettingsEntities = reg.GetEntitiesWith<Components::UISettingsComponent>();
	if (uiSettingsEntities.empty()) {
		return 0;
	}
	auto* uiSettings = reg.Get<Components::UISettingsComponent>(uiSettingsEntities[0]);

	stbtt_bakedchar bakedChars[96]; // ASCII 32 - 127
									// Bake 24pt anti-aliased glyphs into the alpha channel
	int result = stbtt_BakeFontBitmap(fontBuffer.data(), fontOffset, 24.0f, alphaBitmap.data(),
									  atlasSize, atlasSize, 32, 96, bakedChars);

	if (result <= 0) {
		Log("ERROR: stb_truetype failed to bake font bitmap!");
		return 0;
	}

	// Convert 8-bit alpha map into Vulkan-native 32-bit RGBA texture
	std::vector<uint32_t> rgbaPixels(static_cast<size_t>(atlasSize * atlasSize));
	for (uint32_t i = 0; i < atlasSize * atlasSize; ++i) {
		uint8_t alpha = alphaBitmap[i];
		rgbaPixels[i] = (static_cast<uint32_t>(alpha) << 24) | 0x00FFFFFF; // Transparent white
	}

	uint32_t texIdx =
		ctx.CreateTexture(rgbaPixels.data(), atlasSize, atlasSize, false); // Linear sampling

	// Convert Jolt/C++ structures
	for (uint32_t i = 0; i < 96; ++i) {
		const auto& bc = bakedChars[i];
		uiSettings->fontAtlas.glyphs[i] = GlyphMetric{.x0 = static_cast<float>(bc.x0),
													  .y0 = static_cast<float>(bc.y0),
													  .x1 = static_cast<float>(bc.x1),
													  .y1 = static_cast<float>(bc.y1),
													  .xoff = bc.xoff,
													  .yoff = bc.yoff,
													  .xadvance = bc.xadvance};
	}
	uiSettings->fontAtlas.textureIndex = texIdx;
	uiSettings->defaultFontAtlasIdx = texIdx;

	return texIdx;
}

Mesh LoadCookedMesh(RenderContext& ctx, [[maybe_unused]] CreativeWorksManager& assetMgr,
					std::string_view virtualPath) {
	if constexpr (isDev) {
		auto* prefab = LoadModelPrefab(ctx, assetMgr, virtualPath);
		if ((prefab != nullptr) && prefab->partCount > 0) {
			return prefab->parts[0].mesh;
		}
		return {};
	} else {
		CreativeWorkLoadRequest req;
		req.assetID = HashCreativeWorkPath(virtualPath);

		if (!assetMgr.LoadSync(req)) {
			Log("ERROR: Failed to load cooked mesh from PAK: {}", virtualPath);
			return {};
		}

		const auto* header = static_cast<const CookedMeshHeader*>(req.outData);
		if (header->magic != 0x3048534D) {
			Log("ERROR: Invalid CookedMeshHeader magic for: {}", virtualPath);
			return {};
		}

		const char* ptr = reinterpret_cast<const char*>(header + 1);

		BufferHandle posVbo =
			ctx.CreateVertexBuffer(ptr, header->vertexCount * sizeof(VertexPosition));
		ptr += header->vertexCount * sizeof(VertexPosition);

		BufferHandle attrVbo = ctx.CreateVertexBuffer(
			ptr, header->vertexCount * sizeof(VertexAttributes), sizeof(VertexAttributes));
		ptr += header->vertexCount * sizeof(VertexAttributes);

		BufferHandle skinVbo = BufferHandle::Invalid;
		if (header->hasSkin) {
			// FIX: Explicitly pass sizeof(VertexSkin) as the 3rd argument
			skinVbo = ctx.CreateVertexBuffer(ptr, header->vertexCount * sizeof(VertexSkin),
											 sizeof(VertexSkin));
			ptr += header->vertexCount * sizeof(VertexSkin);
		}

		BufferHandle ibo = BufferHandle::Invalid;
		if (header->indexCount > 0) {
			ibo = ctx.CreateIndexBuffer(ptr, header->indexCount * sizeof(uint32_t));
		}

		Mesh finalMesh = Mesh{.posBuffer = posVbo,
							  .attrBuffer = attrVbo,
							  .skinBuffer = skinVbo,
							  .indexBuffer = ibo,
							  .vertexCount = header->vertexCount,
							  .indexCount = header->indexCount};
		ctx.BuildMeshBLAS(finalMesh);
		return finalMesh;
	}
}

uint32_t LoadCookedTexture(RenderContext& ctx, [[maybe_unused]] CreativeWorksManager& assetMgr,
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
		CreativeWorkLoadRequest req;
		req.assetID = HashCreativeWorkPath(virtualPath);

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
			assetMgr.FreeCreativeWorkMemory(req);
			return 0;
		}

		uint32_t textureIndex = ctx.CreateTexture(pixels, width, height);

		stbi_image_free(pixels);
		assetMgr.FreeCreativeWorkMemory(req);

		Log("Loaded Cooked Texture: {} (Bindless Index: {})", virtualPath, textureIndex);
		return textureIndex;
	}
}

} // namespace ZHLN::CreativeWorksFactory
