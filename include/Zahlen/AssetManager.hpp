// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <detail/HashMap.hpp>
#include <detail/Span.hpp>
#include <detail/String.hpp>
#include <string_view>
#include <threading/Mutex.hpp>

namespace ZHLN {

namespace TaskSystem {
struct Counter;
}

struct ModelPrefab;

// ============================================================================
// Hashing Utility
// ============================================================================

constexpr uint64_t HashAssetPath(std::string_view path) noexcept {
	uint64_t hash = 0xcbf29ce484222325ull;
	for (char c : path) {
		hash ^= static_cast<uint64_t>(c);
		hash *= 0x100000001b3ull;
	}
	return hash;
}

// ============================================================================
// Binary Cooked Formats (Aligned to 1-byte packing for disk serialization)
// ============================================================================

#pragma pack(push, 1)

struct PakHeader {
	char magic[4]; // 'Z', 'P', 'A', 'K'
	uint32_t version;
	uint32_t entryCount;
	uint64_t tocOffset; // Absolute offset to the Table of Contents
};

struct PakEntry {
	uint64_t pathHash;		   // FNV-1a Hash of the virtual path
	uint64_t offset;		   // Absolute offset of the payload in the .pak
	uint64_t compressedSize;   // Size on disk
	uint64_t uncompressedSize; // Size in memory
	uint16_t compression;	   // 0 = None, 1 = LZ4, 2 = ZStd
	uint16_t flags;			   // Reserved
};

struct CookedTextureHeader {
	uint32_t magic; // 'T', 'E', 'X', '0'
	uint32_t version;
	uint32_t width;
	uint32_t height;
	uint32_t mipLevels;
	uint32_t vkFormat; // The exact VkFormat required
	uint32_t dataSize; // Size of the raw pixel data following this header
};

struct CookedMeshHeader {
	uint32_t magic; // 'M', 'S', 'H', '0'
	uint32_t version;
	float boundingBoxMin[3];
	float boundingBoxMax[3];
	uint32_t vertexCount;
	uint32_t indexCount;
};

struct CookedAnimHeader {
	uint32_t magic; // 'ANM0'
	uint32_t version;
	float duration;
	uint32_t loop;
	uint32_t trackCount;
};

struct CookedAnimTrack {
	uint64_t targetNodeHash; // Murmur/FNV1a hash of the bone/node name
	uint32_t pathType;		 // 0 = Translation, 1 = Rotation, 2 = Scale
	uint32_t keyCount;
	uint32_t timeOffset;  // Offset to float time array
	uint32_t valueOffset; // Offset to float TRS array
};

#pragma pack(pop)

// ============================================================================
// Asset Manager
// ============================================================================

struct AssetLoadRequest {
	uint64_t assetID = 0;
	void* outData = nullptr;
	size_t outSize = 0;
	bool success = false;
	bool isZeroCopy = false;
};

struct CatalogEntry {
	PakEntry entry;
	struct PakArchive* archive;
};

class AssetManager {
  public:
	AssetManager() = default;
	~AssetManager();

	// Non-copyable
	AssetManager(const AssetManager&) = delete;
	AssetManager& operator=(const AssetManager&) = delete;

	/**
	 * @brief Mounts a .pak file into the virtual file system.
	 * Reads the Table of Contents but does not load payloads into memory.
	 */
	bool MountPak(std::string_view pakFilePath);

	/**
	 * @brief Asynchronously loads a batch of assets using the Fiber TaskSystem.
	 * @param requests Span of requests to fulfill.
	 * @param counter Task counter to wait on.
	 */
	void LoadAsync(RestrictSpan<AssetLoadRequest> requests, TaskSystem::Counter* counter);

	/**
	 * @brief Synchronously loads an asset. Blocks the calling thread/fiber.
	 */
	bool LoadSync(AssetLoadRequest& request);

	/**
	 * @brief Safely frees memory allocated by the AssetManager.
	 */
	void FreeAssetMemory(AssetLoadRequest& req);

	/**
	 * @brief Fetches a cached ModelPrefab, or returns nullptr if not loaded.
	 */
	ModelPrefab* GetCachedPrefab(uint64_t hash);

	// Internal hook for the AssetFactory to register a newly loaded Prefab
	void CachePrefab(uint64_t hash, ModelPrefab* prefab);

  private:
	void ExecuteLoad(AssetLoadRequest* req);

	struct PakArchive** _archives = nullptr;
	size_t _archiveCount = 0;
	size_t _archiveCapacity = 0;

	HashMap<uint64_t, CatalogEntry> _catalog;
	Mutex _catalogMutex;

	// Prefab Cache tracking
	HashMap<uint64_t, ModelPrefab*> _prefabCache;
	ModelPrefab** _prefabsMemory = nullptr;
	size_t _prefabsCount = 0;
	size_t _prefabsCapacity = 0;
	Mutex _prefabMutex;
};

} // namespace ZHLN
