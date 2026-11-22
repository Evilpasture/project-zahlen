// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <AssetCooking/PakWriter.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/FileSystem/VFS.hpp>
#include <cstring>

namespace ZHLN::AssetCooking {

auto PackArchive(std::span<const PakInput> files) -> std::vector<std::byte> {
    std::vector<FS::PakEntry> entries;
    std::vector<std::byte>    payload;

    for (const auto& file: files) {
        const size_t padding = (16 - (payload.size() % 16)) % 16;
        payload.insert(payload.end(), padding, std::byte {0});

        FS::PakEntry pakEntry {};
        pakEntry.pathHash         = HashAssetPath(file.virtualPath);
        pakEntry.offset           = sizeof(FS::PakHeader) + payload.size();
        pakEntry.compressedSize   = file.data.size();
        pakEntry.uncompressedSize = file.data.size();
        entries.push_back(pakEntry);
        payload.insert(payload.end(), file.data.begin(), file.data.end());
    }

    FS::PakHeader header {};
    std::memcpy(header.magic, "ZPAK", 4);
    header.version    = FS::kPakFormatVersion;
    header.entryCount = entries.size();
    header.tocOffset  = sizeof(FS::PakHeader) + payload.size();

    std::vector<std::byte> out;
    const auto append = [&out](const void* data, size_t size) {
        const auto* first = static_cast<const std::byte*>(data);
        out.insert(out.end(), first, first + size);
    };
    append(&header, sizeof(FS::PakHeader));
    out.insert(out.end(), payload.begin(), payload.end());
    if (!entries.empty()) append(entries.data(), entries.size() * sizeof(FS::PakEntry));
    return out;
}

} // namespace ZHLN::AssetCooking
