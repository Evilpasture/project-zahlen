#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Log.hpp>
#include <cstring>
#include <fstream>
#include <new>

// Conditionally compile Zstandard if the library is available in the build environment
#if __has_include(<zstd.h>)
#include <zstd.h>
#define ZHLN_HAS_ZSTD 1
#else
#define ZHLN_HAS_ZSTD 0
#endif

namespace ZHLN {

bool AssetManager::MountPak(const std::string& pakFilePath) {
	std::ifstream file(pakFilePath, std::ios::binary);
	if (!file.is_open()) {
		Log("ERROR: Failed to open PAK file: {}", pakFilePath);
		return false;
	}

	PakHeader header{};
	file.read(reinterpret_cast<char*>(&header), sizeof(PakHeader));

	if (std::memcmp(header.magic, "ZPAK", 4) != 0) {
		Log("ERROR: Invalid PAK magic signature in: {}", pakFilePath);
		return false;
	}

	// Seek to the Table of Contents at the end of the file
	file.seekg(header.tocOffset, std::ios::beg);

	std::vector<PakEntry> entries(header.entryCount);
	file.read(reinterpret_cast<char*>(entries.data()), header.entryCount * sizeof(PakEntry));

	// Register the archive
	auto archive = std::make_unique<PakArchive>();
	archive->path = pakFilePath;
	PakArchive* archivePtr = archive.get();

	std::lock_guard<std::mutex> lock(_catalogMutex);
	_archives.push_back(std::move(archive));

	// Populate global catalog
	for (const auto& entry : entries) {
		_catalog[entry.pathHash] = {entry, archivePtr};
	}

	Log("Mounted PAK: {} ({} assets)", pakFilePath, header.entryCount);
	return true;
}

void AssetManager::LoadAsync(std::span<AssetLoadRequest> requests, TaskSystem::Counter* counter) {
	if (requests.empty()) {
		return;
	}

	std::vector<TaskSystem::Task> tasks;
	tasks.reserve(requests.size());

	for (auto& req : requests) {
		// Allocate a small payload for the fiber to execute
		auto* jobPayload = new std::pair<AssetManager*, AssetLoadRequest*>(this, &req);

		tasks.push_back({.func = [](void* arg) -> void {
							 auto* payload =
								 static_cast<std::pair<AssetManager*, AssetLoadRequest*>*>(arg);
							 payload->first->ExecuteLoad(payload->second);
							 delete payload; // Cleanup the allocation after execution
						 },
						 .arg = jobPayload});
	}

	// The TaskSystem automatically increments/decrements the counter!
	TaskSystem::Dispatch(tasks, counter);
}

bool AssetManager::LoadSync(AssetLoadRequest& request) {
	ExecuteLoad(&request);
	return request.success;
}

void AssetManager::FreeAssetMemory(void* data) {
	if (data) {
		// Memory was allocated with 16-byte alignment for GPU transfer safety
		::operator delete[](data, std::align_val_t{16});
	}
}

void AssetManager::ExecuteLoad(AssetLoadRequest* req) {
	PakEntry entry;
	PakArchive* archive = nullptr;

	{
		std::lock_guard<std::mutex> lock(_catalogMutex);
		auto it = _catalog.find(req->assetID);
		if (it == _catalog.end()) {
			req->success = false;
			return;
		}
		entry = it->second.first;
		archive = it->second.second;
	}

	// Allocate 16-byte aligned memory to satisfy strict GPU upload requirements
	req->outSize = entry.uncompressedSize;
	req->outData = ::operator new[](entry.uncompressedSize, std::align_val_t{16});

	std::vector<char> compressedBuffer;
	char* readTarget = static_cast<char*>(req->outData);

	if (entry.compression != 0) {
		compressedBuffer.resize(entry.compressedSize);
		readTarget = compressedBuffer.data();
	}

	// --- Thread-Safe Disk I/O ---
	// Note: To prevent thrashing the disk head on HDDs, you might eventually replace this
	// with OS-level async I/O (Overlapped/io_uring), but mutex-protected streams work great for
	// SSDs.
	{
		std::lock_guard<std::mutex> lock(archive->mtx);
		std::ifstream file(archive->path, std::ios::binary);
		file.seekg(entry.offset, std::ios::beg);
		file.read(readTarget, entry.compressedSize);
	}

	// --- Decompression ---
	if (entry.compression == 2) { // ZStandard
#if ZHLN_HAS_ZSTD
		size_t result = ZSTD_decompress(req->outData, entry.uncompressedSize,
										compressedBuffer.data(), entry.compressedSize);

		if (ZSTD_isError(result)) {
			Log("ERROR: Zstd decompression failed for asset ID: {:X}", req->assetID);
			FreeAssetMemory(req->outData);
			req->outData = nullptr;
			req->success = false;
			return;
		}
#else
		Log("FATAL: Asset requires Zstd decompression, but Engine was built without Zstd support!");
		FreeAssetMemory(req->outData);
		req->outData = nullptr;
		req->success = false;
		return;
#endif
	} else if (entry.compression == 1) { // LZ4 placeholder
		Log("ERROR: LZ4 compression not currently implemented.");
		FreeAssetMemory(req->outData);
		req->outData = nullptr;
		req->success = false;
		return;
	}

	req->success = true;
}

} // namespace ZHLN
