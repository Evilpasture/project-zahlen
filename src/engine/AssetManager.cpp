#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Log.hpp>
#include <cstring>
#include <engine/Platform.hpp>
#include <new>

#if __has_include(<zstd.h>)
#include <zstd.h>
#define ZHLN_HAS_ZSTD 1
#else
#define ZHLN_HAS_ZSTD 0
#endif

namespace ZHLN {

bool AssetManager::MountPak(const std::string& pakFilePath) {
	auto archive = std::make_unique<PakArchive>();
	archive->path = pakFilePath;
	archive->mapped = Platform::OpenMappedFile(pakFilePath.c_str());

	if (archive->mapped.data == nullptr) {
		Log("ERROR: Failed to map PAK file: {}", pakFilePath);
		return false;
	}

	const auto* header = static_cast<const PakHeader*>(archive->mapped.data);
	if (std::memcmp(header->magic, "ZPAK", 4) != 0) {
		Log("ERROR: Invalid PAK magic signature in: {}", pakFilePath);
		Platform::CloseMappedFile(archive->mapped);
		return false;
	}

	const auto* entries = reinterpret_cast<const PakEntry*>(
		static_cast<const char*>(archive->mapped.data) + header->tocOffset);

	PakArchive* archivePtr = archive.get();
	std::lock_guard<std::mutex> lock(_catalogMutex);
	_archives.push_back(std::move(archive));

	for (uint32_t i = 0; i < header->entryCount; ++i) {
		_catalog[entries[i].pathHash] = {entries[i], archivePtr};
	}

	Log("Mounted PAK: {} ({} assets)", pakFilePath, header->entryCount);
	return true;
}

void AssetManager::LoadAsync(std::span<AssetLoadRequest> requests, TaskSystem::Counter* counter) {
	if (requests.empty()) {
		return;
	}

	std::vector<TaskSystem::Task> tasks;
	tasks.reserve(requests.size());

	for (auto& req : requests) {
		auto* jobPayload = new std::pair<AssetManager*, AssetLoadRequest*>(this, &req);

		tasks.push_back({.func = [](void* arg) -> void {
							 auto* payload =
								 static_cast<std::pair<AssetManager*, AssetLoadRequest*>*>(arg);
							 payload->first->ExecuteLoad(payload->second);
							 delete payload;
						 },
						 .arg = jobPayload});
	}

	TaskSystem::Dispatch(tasks, counter);
}

bool AssetManager::LoadSync(AssetLoadRequest& request) {
	ExecuteLoad(&request);
	return request.success;
}

void AssetManager::ExecuteLoad(AssetLoadRequest* req) {
	PakEntry entry{};
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

	req->outSize = entry.uncompressedSize;
	const char* payloadRaw = static_cast<const char*>(archive->mapped.data) + entry.offset;

	if (entry.compression == 0) {
		// --- ZERO-COPY FAST PATH ---
		req->outData = const_cast<char*>(payloadRaw);
		req->isZeroCopy = true;
		req->success = true;
		return;
	}

	// --- DECOMPRESSION PATH ---
	req->outData = ::operator new[](entry.uncompressedSize, std::align_val_t{16});
	req->isZeroCopy = false;

	if (entry.compression == 2) { // ZStandard
#if ZHLN_HAS_ZSTD
		size_t result =
			ZSTD_decompress(req->outData, entry.uncompressedSize, payloadRaw, entry.compressedSize);
		if (ZSTD_isError(result)) {
			Log("ERROR: Zstd decompression failed for asset ID: {:X}", req->assetID);
			FreeAssetMemory(*req);
			req->success = false;
			return;
		}
#else
		Log("FATAL: Engine built without ZStd support! Cannot decompress Asset.");
		FreeAssetMemory(*req);
		req->success = false;
		return;
#endif
	} else if (entry.compression == 1) { // LZ4 Placeholder
		Log("ERROR: LZ4 compression not currently implemented.");
		FreeAssetMemory(*req);
		req->success = false;
		return;
	}

	req->success = true;
}

void AssetManager::FreeAssetMemory(AssetLoadRequest& req) {
	if (!req.isZeroCopy && (req.outData != nullptr)) {
		::operator delete[](req.outData, std::align_val_t{16});
	}
	req.outData = nullptr;
}

} // namespace ZHLN
