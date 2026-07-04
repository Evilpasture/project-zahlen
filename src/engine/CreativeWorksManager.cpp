// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "threading/TaskSystem.hpp"

#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <cgltf.h>
#include <cstring>
#include <detail/ControlFlow.hpp>
#include <engine/Platform.hpp>
#include <new>
#include <vector>

#if __has_include(<zstd.h>)
#include <zstd.h>
#define ZHLN_HAS_ZSTD 1
#else
#define ZHLN_HAS_ZSTD 0
#endif

namespace ZHLN {

struct PakArchive {
	String256 path;
	Platform::MappedFile mapped;
};

CreativeWorksManager::~CreativeWorksManager() {
	for (size_t i = 0; i < _archiveCount; ++i) {
		Platform::CloseMappedFile(_archives[i]->mapped);
		delete _archives[i];
	}
	for (size_t i = 0; i < _prefabsCount; ++i) {
		auto* prefab = _prefabsMemory[i];
		if (prefab->rawData != nullptr) {
			cgltf_free(prefab->rawData);
		}
		delete prefab;
	}
	delete[] _prefabsMemory;
	delete[] _archives;
}

bool CreativeWorksManager::MountPak(std::string_view pakFilePath) {
	auto* archive = new PakArchive();
	archive->path = pakFilePath;
	archive->mapped = Platform::OpenMappedFile(archive->path.c_str());

	if (archive->mapped.data == nullptr) {
		Log("ERROR: Failed to map PAK file: {}", pakFilePath);
		delete archive;
		return false;
	}

	const auto* header = static_cast<const PakHeader*>(archive->mapped.data);
	if (std::memcmp(header->magic, "ZPAK", 4) != 0) {
		Log("ERROR: Invalid PAK magic signature in: {}", pakFilePath);
		Platform::CloseMappedFile(archive->mapped);
		delete archive;
		return false;
	}

	const auto* entries = reinterpret_cast<const PakEntry*>(
		static_cast<const char*>(archive->mapped.data) + header->tocOffset);

	ZHLN_LOCK(_catalogMutex) {
		if (_archiveCount >= _archiveCapacity) {
			size_t newCap = _archiveCapacity == 0 ? 4 : _archiveCapacity * 2;
			auto** newArrs = new PakArchive*[newCap];
			if (_archives != nullptr) {
				std::memcpy(newArrs, _archives, _archiveCount * sizeof(PakArchive*));
				delete[] _archives;
			}
			_archives = newArrs;
			_archiveCapacity = newCap;
		}
		_archives[_archiveCount++] = archive;

		for (uint32_t i = 0; i < header->entryCount; ++i) {
			_catalog.Insert(entries[i].pathHash, CatalogEntry{entries[i], archive});
		}
	}

	Log("Mounted PAK: {} ({} assets)", pakFilePath, header->entryCount);
	return true;
}

void CreativeWorksManager::LoadAsync(RestrictSpan<CreativeWorkLoadRequest> requests,
							 TaskSystem::Counter* counter) {
	if (requests.size() == 0) {
		return;
	}

	std::vector<TaskSystem::Task> tasks;
	tasks.reserve(requests.size());

	for (auto& request : requests) {
		auto* jobPayload = new std::pair<CreativeWorksManager*, CreativeWorkLoadRequest*>(this, &request);

		tasks.push_back({.func = [](void* arg) -> void {
							 auto* payload =
								 static_cast<std::pair<CreativeWorksManager*, CreativeWorkLoadRequest*>*>(arg);
							 payload->first->ExecuteLoad(payload->second);
							 delete payload;
						 },
						 .arg = jobPayload});
	}

	TaskSystem::Dispatch(tasks, counter);
}

bool CreativeWorksManager::LoadSync(CreativeWorkLoadRequest& request) {
	ExecuteLoad(&request);
	return request.success;
}

void CreativeWorksManager::ExecuteLoad(CreativeWorkLoadRequest* req) {
	PakEntry entry{};
	PakArchive* archive = nullptr;

	{
		ZHLN_LOCK(_catalogMutex) {
			const CatalogEntry* catEntry = _catalog.Find(req->assetID);
			if (catEntry == nullptr) {
				req->success = false;
				return;
			}
			entry = catEntry->entry;
			archive = catEntry->archive;
		}
	}

	req->outSize = entry.uncompressedSize;
	const char* payloadRaw = static_cast<const char*>(archive->mapped.data) + entry.offset;

	if (entry.compression == 0) {
		req->outData = const_cast<char*>(payloadRaw);
		req->isZeroCopy = true;
		req->success = true;
		return;
	}

	req->outData = ::operator new[](entry.uncompressedSize, std::align_val_t{16});
	req->isZeroCopy = false;

	if (entry.compression == 2) { // ZStandard
#if ZHLN_HAS_ZSTD
		size_t result =
			ZSTD_decompress(req->outData, entry.uncompressedSize, payloadRaw, entry.compressedSize);
		if (ZSTD_isError(result)) {
			Log("ERROR: Zstd decompression failed for asset ID: {:X}", req->assetID);
			FreeCreativeWorkMemory(*req);
			req->success = false;
			return;
		}
#else
		Log("FATAL: Engine built without ZStd support! Cannot decompress CreativeWork.");
		FreeCreativeWorkMemory(*req);
		req->success = false;
		return;
#endif
	} else if (entry.compression == 1) { // LZ4 Placeholder
		Log("ERROR: LZ4 compression not currently implemented.");
		FreeCreativeWorkMemory(*req);
		req->success = false;
		return;
	}

	req->success = true;
}

void CreativeWorksManager::FreeCreativeWorkMemory(CreativeWorkLoadRequest& req) {
	if (!req.isZeroCopy && (req.outData != nullptr)) {
		::operator delete[](req.outData, std::align_val_t{16});
	}
	req.outData = nullptr;
}

ModelPrefab* CreativeWorksManager::GetCachedPrefab(uint64_t hash) {
	ZHLN_LOCK(_prefabMutex) {
		const auto* entry = _prefabCache.Find(hash);
		if (entry != nullptr) {
			return *entry;
		}
		return nullptr;
	}
}

void CreativeWorksManager::CachePrefab(uint64_t hash, ModelPrefab* prefab) {
	ZHLN_LOCK(_prefabMutex) {
		_prefabCache.Insert(hash, prefab);
		if (_prefabsCount >= _prefabsCapacity) {
			size_t newCap = _prefabsCapacity == 0 ? 8 : _prefabsCapacity * 2;
			auto** newArrs = new ModelPrefab*[newCap];
			if (_prefabsMemory != nullptr) {
				std::memcpy(newArrs, _prefabsMemory, _prefabsCount * sizeof(ModelPrefab*));
				delete[] _prefabsMemory;
			}
			_prefabsMemory = newArrs;
			_prefabsCapacity = newCap;
		}
		_prefabsMemory[_prefabsCount++] = prefab;
	}
}

void CreativeWorksManager::ClearCache() noexcept {
	ZHLN_LOCK(_prefabMutex) {
		// 1. Clear the lookup hash map
		_prefabCache.Clear();

		// 2. Destroy and free all memory allocated for cached prefabs
		for (size_t i = 0; i < _prefabsCount; ++i) {
			auto* prefab = _prefabsMemory[i];
			if (prefab->rawData != nullptr) {
				cgltf_free(prefab->rawData);
			}
			delete prefab;
		}
		_prefabsCount = 0;
	}
}

uint32_t CreativeWorksManager::GetCachedPrefabs(ModelPrefab** outPrefabs, uint32_t maxCount) {
	ZHLN_LOCK(_prefabMutex) {
		if (outPrefabs == nullptr || maxCount == 0) {
			return static_cast<uint32_t>(_prefabsCount);
		}
		uint32_t toCopy = std::min(static_cast<uint32_t>(_prefabsCount), maxCount);
		std::memcpy(outPrefabs, _prefabsMemory, toCopy * sizeof(ModelPrefab*));
		return toCopy;
	}
}

} // namespace ZHLN
