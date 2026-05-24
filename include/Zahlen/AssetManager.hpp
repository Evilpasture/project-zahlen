#pragma once

#include "engine/Platform.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <threading/TaskSystem.hpp>
#include <unordered_map>
#include <vector>

namespace ZHLN {

// ============================================================================
// Hashing Utility
// ============================================================================

// Compile-time FNV-1a 64-bit hash for converting string paths to AssetIDs
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
	// Followed by Vertex array, then uint32_t Index array
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

class AssetManager {
  public:
	AssetManager() = default;
	~AssetManager() = default;

	// Non-copyable
	AssetManager(const AssetManager&) = delete;
	AssetManager& operator=(const AssetManager&) = delete;

	/**
	 * @brief Mounts a .pak file into the virtual file system.
	 * Reads the Table of Contents but does not load payloads into memory.
	 */
	bool MountPak(const std::string& pakFilePath);

	/**
	 * @brief Asynchronously loads a batch of assets using the Fiber TaskSystem.
	 * @param requests Span of requests to fulfill.
	 * @param counter Task counter to wait on.
	 */
	void LoadAsync(std::span<AssetLoadRequest> requests, TaskSystem::Counter* counter);

	/**
	 * @brief Synchronously loads an asset. Blocks the calling thread/fiber.
	 */
	bool LoadSync(AssetLoadRequest& request);

	/**
	 * @brief Safely frees memory allocated by the AssetManager.
	 */
	void FreeAssetMemory(AssetLoadRequest& req);

  private:
	struct PakArchive {
		std::string path;
		Platform::MappedFile mapped;
	};

	void ExecuteLoad(AssetLoadRequest* req);

	std::vector<std::unique_ptr<PakArchive>> _archives;
	std::unordered_map<uint64_t, std::pair<PakEntry, PakArchive*>> _catalog;
	std::mutex _catalogMutex;
};

} // namespace ZHLN
