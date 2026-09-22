// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/filesystem/VirtualFileSystem.cpp
//
// Low-level VFS / PakArchive manager. Pure binary I/O, no Mesh/Texture/Prefab
// knowledge. Extracted from AssetManager.cpp to zahlen_filesystem so
// that zcook and tests can mount .pak without linking zahlen_engine
// (Jolt/Vulkan/ECS).

#include <Zahlen/FileSystem/VFS.hpp>
#include <Zahlen/FileSystem/MappedFile.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/String.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <vector>

#if __has_include(<zstd.h>)
#include <zstd.h>
#define ZHLN_HAS_ZSTD 1
#else
#define ZHLN_HAS_ZSTD 0
#endif

namespace ZHLN::FS {

struct PakArchive {
    String256 path;
    MappedFile mapped;
};

VirtualFileSystem::~VirtualFileSystem() {
    for (size_t i = 0; i < _archiveCount; ++i) {
        if (_archives[i] != nullptr) {
            CloseMappedFile(_archives[i]->mapped);
            delete _archives[i];
        }
    }
    delete[] _archives;
}

bool VirtualFileSystem::MountDirectory(std::string_view directory) {
    if (directory.empty()) {
        return false;
    }
    return Lock(_mountDirMutex, [&]() -> bool {
        if (_mountDirCount >= kMaxMountDirs) {
            return false;
        }
        _mountDirs[_mountDirCount++] = std::string(directory);
        return true;
    });
}

bool VirtualFileSystem::MountPak(std::string_view pakFilePath) {
    if (pakFilePath.empty()) {
        return false;
    }

    auto* archive = new (std::nothrow) PakArchive();
    if (archive == nullptr) {
        return false;
    }
    archive->path.assign(pakFilePath);
    archive->mapped = OpenMappedFile(archive->path.c_str());

    if (archive->mapped.data == nullptr) {
        delete archive;
        return false;
    }

    if (archive->mapped.size < sizeof(PakHeader)) {
        CloseMappedFile(archive->mapped);
        delete archive;
        return false;
    }

    PakHeader header {};
    std::memcpy(&header, archive->mapped.data, sizeof(PakHeader));

    if (std::memcmp(header.magic, "ZPAK", 4) != 0) {
        CloseMappedFile(archive->mapped);
        delete archive;
        return false;
    }

    // --- Validate header to avoid SIGBUS on corrupt/truncated paks ---
    // Version 2 is the packed .pak ABI (see Zahlen/FileSystem/VFS.hpp). A
    // version-1 archive pads the header to 24 bytes and every TOC entry to 40,
    // so reading one with the current structs would land tocOffset on the wrong
    // bytes; rejecting it here is what turns that into a clean mount failure.
    if (header.version != kPakFormatVersion) {
        CloseMappedFile(archive->mapped);
        delete archive;
        return false;
    }
    if (header.entryCount > 1'000'000) {
        CloseMappedFile(archive->mapped);
        delete archive;
        return false;
    }
    if (header.tocOffset < sizeof(PakHeader) || header.tocOffset > archive->mapped.size) {
        CloseMappedFile(archive->mapped);
        delete archive;
        return false;
    }
    const size_t tocBytes = static_cast<size_t>(header.entryCount) * sizeof(PakEntry);
    if (header.tocOffset + tocBytes > archive->mapped.size) {
        CloseMappedFile(archive->mapped);
        delete archive;
        return false;
    }

    const auto* baseData = static_cast<const char*>(archive->mapped.data);

    // Pre-validate each entry's payload range
    for (uint32_t i = 0; i < header.entryCount; ++i) {
        PakEntry tmp {};
        std::memcpy(&tmp, baseData + header.tocOffset + (i * sizeof(PakEntry)), sizeof(PakEntry));
        if (tmp.offset + tmp.compressedSize > archive->mapped.size) {
            CloseMappedFile(archive->mapped);
            delete archive;
            return false;
        }
        if (tmp.offset < sizeof(PakHeader) || tmp.offset > header.tocOffset) {
            CloseMappedFile(archive->mapped);
            delete archive;
            return false;
        }
        if (tmp.uncompressedSize > (100ULL << 20)) { // 100 MiB sanity
            CloseMappedFile(archive->mapped);
            delete archive;
            return false;
        }
    }

    bool ok = Lock(_catalogMutex, [&]() -> bool {
        if (_archiveCount >= _archiveCapacity) {
            size_t newCap = _archiveCapacity == 0 ? 4 : _archiveCapacity * 2;
            auto** newArrs = new (std::nothrow) PakArchive*[newCap];
            if (newArrs == nullptr) {
                return false;
            }
            if (_archives != nullptr) {
                std::memcpy(static_cast<void*>(newArrs), static_cast<void*>(_archives), _archiveCount * sizeof(PakArchive*));
                delete[] _archives;
            }
            _archives        = newArrs;
            _archiveCapacity = newCap;
        }
        _archives[_archiveCount++] = archive;

        for (uint32_t i = 0; i < header.entryCount; ++i) {
            PakEntry entry {};
            std::memcpy(&entry, baseData + header.tocOffset + (i * sizeof(PakEntry)), sizeof(PakEntry));
            _catalog.Insert(entry.pathHash, CatalogEntry {.entry = entry, .archive = archive});
        }
        return true;
    });

    if (!ok) {
        CloseMappedFile(archive->mapped);
        delete archive;
        return false;
    }

    return true;
}

void VirtualFileSystem::LoadAsync(RestrictSpan<LoadRequest> requests, ::ZHLN::TaskSystem::Counter* counter) {
    if (requests.size() == 0) {
        return;
    }

    std::vector<::ZHLN::TaskSystem::Task> tasks;
    tasks.reserve(requests.size());

    for (auto& request : requests) {
        auto* jobPayload = new std::pair<VirtualFileSystem*, LoadRequest*>(this, &request);

        tasks.push_back(
            {.func = [](void* arg) -> void {
                 auto* payload = static_cast<std::pair<VirtualFileSystem*, LoadRequest*>*>(arg);
                 payload->first->ExecuteLoad(payload->second);
                 delete payload;
             },
             .arg = jobPayload}
        );
    }

    ::ZHLN::TaskSystem::Dispatch(tasks, counter);
}

bool VirtualFileSystem::LoadSync(LoadRequest& request) {
    ExecuteLoad(&request);
    return request.success;
}

bool VirtualFileSystem::TryLoadFromDirectories(LoadRequest* req) const {
    (void)req;
    return false;
}

void VirtualFileSystem::ExecuteLoad(LoadRequest* req) {
    if (req == nullptr) {
        return;
    }

    PakEntry    entry {};
    PakArchive* archive = nullptr;

    {
        Lock(_catalogMutex, [&] {
            const CatalogEntry* catEntry = _catalog.Find(req->assetID);
            if (catEntry == nullptr) {
                return;
            }
            entry   = catEntry->entry;
            archive = catEntry->archive;
        });
    }

    if (archive == nullptr || archive->mapped.data == nullptr) {
        req->success = false;
        return;
    }

    if (entry.offset + entry.compressedSize > archive->mapped.size) {
        req->success = false;
        return;
    }

    req->outSize     = entry.uncompressedSize;
    char* payloadRaw = static_cast<char*>(archive->mapped.data) + entry.offset;

    if (entry.compression == 0) {
        req->outData    = payloadRaw;
        req->isZeroCopy = true;
        req->success    = true;
        return;
    }

    req->outData    = ::operator new[](entry.uncompressedSize, std::align_val_t {16});
    req->isZeroCopy = false;

    if (entry.compression == 2) { // ZStandard
#if ZHLN_HAS_ZSTD
        size_t result = ZSTD_decompress(req->outData, entry.uncompressedSize, payloadRaw, entry.compressedSize);
        if (ZSTD_isError(result)) {
            FreeMemory(*req);
            req->success = false;
            return;
        }
#else
        FreeMemory(*req);
        req->success = false;
        return;
#endif
    } else if (entry.compression == 1) { // LZ4 Placeholder
        FreeMemory(*req);
        req->success = false;
        return;
    }

    req->success = true;
}

void VirtualFileSystem::FreeMemory(LoadRequest& req) {
    if (!req.isZeroCopy && (req.outData != nullptr)) {
        ::operator delete[](req.outData, std::align_val_t {16});
    }
    req.outData = nullptr;
}

auto VirtualFileSystem::Exists(uint64_t assetID) const noexcept -> bool {
    bool inPak = Lock(_catalogMutex, [&]() -> bool {
        return _catalog.Find(assetID) != nullptr;
    });
    return inPak;
}

auto VirtualFileSystem::ReadFile(std::string_view virtualPath, void* outData, size_t outCapacity) const -> size_t {
    // Try mounted directories first (dev mode)
    auto tryDirs = Lock(_mountDirMutex, [&]() -> size_t {
        for (size_t i = 0; i < _mountDirCount; ++i) {
            std::filesystem::path full = std::filesystem::path(_mountDirs[i]) / virtualPath;
            std::error_code ec;
            auto sz = std::filesystem::file_size(full, ec);
            if (ec) {
                continue;
            }
            if (outData == nullptr) {
                return static_cast<size_t>(sz);
            }
            if (sz > outCapacity) {
                return 0; // buffer too small
            }
            std::ifstream f(full, std::ios::binary);
            if (!f) {
                continue;
            }
            f.read(static_cast<char*>(outData), static_cast<std::streamsize>(sz));
            if (!f) {
                return 0;
            }
            return static_cast<size_t>(sz);
        }
        return 0;
    });
    if (tryDirs != 0) {
        return tryDirs;
    }

    // Fallback to pak via assetID
    uint64_t id = Hash64(virtualPath);
    const CatalogEntry* catEntry = nullptr;
    PakArchive* archive = nullptr;
    PakEntry entry {};
    Lock(_catalogMutex, [&] {
        catEntry = _catalog.Find(id);
        if (catEntry != nullptr) {
            entry = catEntry->entry;
            archive = catEntry->archive;
        }
    });
    if (archive == nullptr || archive->mapped.data == nullptr) {
        return 0;
    }
    if (entry.offset + entry.compressedSize > archive->mapped.size) {
        return 0;
    }
    if (outData == nullptr) {
        return static_cast<size_t>(entry.uncompressedSize);
    }
    if (entry.uncompressedSize > outCapacity) {
        return 0;
    }
    if (entry.compression == 0) {
        std::memcpy(outData, static_cast<char*>(archive->mapped.data) + entry.offset, entry.uncompressedSize);
        return static_cast<size_t>(entry.uncompressedSize);
    }
    char* payloadRaw = static_cast<char*>(archive->mapped.data) + entry.offset;
    if (entry.compression == 2) {
#if ZHLN_HAS_ZSTD
        size_t res = ZSTD_decompress(outData, outCapacity, payloadRaw, entry.compressedSize);
        if (ZSTD_isError(res)) {
            return 0;
        }
        return res;
#else
        return 0;
#endif
    }
    return 0;
}

} // namespace ZHLN::FS
