// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SourceVFS.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>

namespace ZHLN::BSP {

auto SourceVFS::NormalizePath(std::string_view path) -> std::string {
    std::string out;
    out.reserve(path.size());

    // Skip leading slashes or "./"
    size_t start = 0;
    while (start < path.size() && (path[start] == '/' || path[start] == '\\')) {
        start++;
    }
    if (start + 1 < path.size() && path[start] == '.' && (path[start + 1] == '/' || path[start + 1] == '\\')) {
        start += 2;
    }

    for (size_t i = start; i < path.size(); ++i) {
        char c = path[i];
        if (c == '\\') {
            c = '/';
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    // Collapse multiple consecutive slashes
    std::string collapsed;
    collapsed.reserve(out.size());
    bool lastSlash = false;
    for (char c: out) {
        if (c == '/') {
            if (!lastSlash) {
                collapsed.push_back(c);
            }
            lastSlash = true;
        } else {
            collapsed.push_back(c);
            lastSlash = false;
        }
    }
    return collapsed;
}

void SourceVFS::AddSearchPath(std::string_view path) {
    if (path.empty()) {
        return;
    }
    std::string p(path);
    if (std::find(_searchPaths.begin(), _searchPaths.end(), p) == _searchPaths.end()) {
        _searchPaths.push_back(p);
        _indexed = false;
    }
}

void SourceVFS::IndexDirectory(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return;
    }

    for (const auto& entry: std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_regular_file(ec)) {
            const auto relPath = std::filesystem::relative(entry.path(), root, ec);
            if (!ec) {
                const std::string norm = NormalizePath(relPath.generic_string());
                if (!_diskIndex.contains(norm)) {
                    _diskIndex[norm] = entry.path().string();
                }
            }
        }
    }
}

void SourceVFS::IndexPakfile(std::span<const std::byte> pakData) {
    if (pakData.size() < 22) {
        return;
    }

    _pakDataCopy.assign(pakData.begin(), pakData.end());

    // Search for ZIP End of Central Directory (EOCD) signature: 0x06054b50
    const uint8_t* bytes    = reinterpret_cast<const uint8_t*>(_pakDataCopy.data());
    const size_t   totalLen = _pakDataCopy.size();

    int64_t eocdPos = -1;
    const size_t maxSearch = std::min(totalLen, size_t {65557});
    for (size_t i = totalLen - 22; i + maxSearch >= totalLen && i > 0; --i) {
        if (bytes[i] == 0x50 && bytes[i + 1] == 0x4B && bytes[i + 2] == 0x05 && bytes[i + 3] == 0x06) {
            eocdPos = static_cast<int64_t>(i);
            break;
        }
    }

    if (eocdPos < 0) {
        return;
    }

    const uint8_t* eocd = bytes + eocdPos;
    uint32_t cdOffset = 0;
    std::memcpy(&cdOffset, eocd + 16, 4);
    uint16_t numEntries = 0;
    std::memcpy(&numEntries, eocd + 10, 2);

    if (cdOffset >= totalLen) {
        return;
    }

    const uint8_t* cur = bytes + cdOffset;
    for (uint16_t k = 0; k < numEntries && (cur + 46 <= bytes + totalLen); ++k) {
        if (cur[0] != 0x50 || cur[1] != 0x4B || cur[2] != 0x01 || cur[3] != 0x02) {
            break;
        }

        uint16_t method = 0;
        std::memcpy(&method, cur + 10, 2);
        uint32_t compSize = 0;
        std::memcpy(&compSize, cur + 20, 4);
        uint32_t uncompSize = 0;
        std::memcpy(&uncompSize, cur + 24, 4);
        uint16_t nameLen = 0;
        std::memcpy(&nameLen, cur + 28, 2);
        uint16_t extraLen = 0;
        std::memcpy(&extraLen, cur + 30, 2);
        uint16_t commentLen = 0;
        std::memcpy(&commentLen, cur + 32, 2);
        uint32_t localHeaderOffset = 0;
        std::memcpy(&localHeaderOffset, cur + 42, 4);

        if (cur + 46 + nameLen > bytes + totalLen) {
            break;
        }

        std::string fileName(reinterpret_cast<const char*>(cur + 46), nameLen);
        const std::string normName = NormalizePath(fileName);

        // Calculate actual data offset from local file header
        if (localHeaderOffset + 30 <= totalLen) {
            const uint8_t* localHeader = bytes + localHeaderOffset;
            if (localHeader[0] == 0x50 && localHeader[1] == 0x4B && localHeader[2] == 0x03 && localHeader[3] == 0x04) {
                uint16_t localNameLen = 0;
                std::memcpy(&localNameLen, localHeader + 26, 2);
                uint16_t localExtraLen = 0;
                std::memcpy(&localExtraLen, localHeader + 28, 2);
                const uint32_t dataOffset = localHeaderOffset + 30 + localNameLen + localExtraLen;

                _pakIndex[normName] = PakEntry {
                    .offset     = dataOffset,
                    .compSize   = compSize,
                    .uncompSize = uncompSize,
                    .method     = method,
                };
            }
        }

        cur += 46 + nameLen + extraLen + commentLen;
    }
}

void SourceVFS::SetPakfileData(std::span<const std::byte> pakData) {
    _pakIndex.clear();
    IndexPakfile(pakData);
}

auto SourceVFS::ResolveFile(std::string_view virtualPath) const -> std::optional<std::string> {
    if (virtualPath.empty()) {
        return std::nullopt;
    }

    // 1. Direct check
    std::error_code ec;
    if (std::filesystem::exists(virtualPath, ec) && std::filesystem::is_regular_file(virtualPath, ec)) {
        return std::string(virtualPath);
    }

    const std::string norm = NormalizePath(virtualPath);

    // 2. Check indexed disk cache
    if (!_indexed) {
        for (const auto& searchPath: _searchPaths) {
            const_cast<SourceVFS*>(this)->IndexDirectory(searchPath);
        }
        _indexed = true;
    }

    if (auto it = _diskIndex.find(norm); it != _diskIndex.end()) {
        return it->second;
    }

    // 3. Fallback: try prefixing with each search path directly
    for (const auto& searchPath: _searchPaths) {
        std::filesystem::path candidate = std::filesystem::path(searchPath) / virtualPath;
        if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec)) {
            _diskIndex[norm] = candidate.string();
            return candidate.string();
        }
    }

    return std::nullopt;
}

auto SourceVFS::Exists(std::string_view virtualPath) const -> bool {
    const std::string norm = NormalizePath(virtualPath);
    if (_pakIndex.contains(norm)) {
        return true;
    }
    return ResolveFile(virtualPath).has_value();
}

auto SourceVFS::ReadFile(std::string_view virtualPath) const -> std::optional<std::vector<std::byte>> {
    const std::string norm = NormalizePath(virtualPath);

    // 1. Check embedded pakfile
    if (auto it = _pakIndex.find(norm); it != _pakIndex.end()) {
        const auto& entry = it->second;
        if (entry.offset + entry.compSize <= _pakDataCopy.size()) {
            if (entry.method == 0) { // Store (uncompressed)
                std::vector<std::byte> result(entry.uncompSize);
                std::memcpy(result.data(), _pakDataCopy.data() + entry.offset, entry.uncompSize);
                return result;
            }
        }
    }

    // 2. Check disk
    const auto resolved = ResolveFile(virtualPath);
    if (!resolved.has_value()) {
        return std::nullopt;
    }

    std::ifstream file(*resolved, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return std::vector<std::byte> {};
    }
    file.seekg(0);
    std::vector<std::byte> buffer(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return std::nullopt;
    }
    return buffer;
}

auto SourceVFS::ResolveMaterial(std::string_view materialName) const -> std::optional<std::string> {
    if (materialName.empty()) {
        return std::nullopt;
    }

    std::string cleanName(materialName);
    if (cleanName.starts_with("materials/") || cleanName.starts_with("materials\\")) {
        cleanName = cleanName.substr(10);
    }

    // Candidate 1: materials/<name>.vmt
    std::string candidate = "materials/" + cleanName;
    if (!candidate.ends_with(".vmt")) {
        candidate += ".vmt";
    }
    if (Exists(candidate)) {
        return candidate;
    }

    // Candidate 2: <name>.vmt
    std::string cand2 = cleanName;
    if (!cand2.ends_with(".vmt")) {
        cand2 += ".vmt";
    }
    if (Exists(cand2)) {
        return cand2;
    }

    // Candidate 3: materials/<name>.png
    std::string candPng = "materials/" + cleanName;
    if (candPng.ends_with(".vmt")) {
        candPng = candPng.substr(0, candPng.size() - 4);
    }
    candPng += ".png";
    if (Exists(candPng)) {
        return candPng;
    }

    return std::nullopt;
}

auto SourceVFS::ResolveTexture(std::string_view textureName) const -> std::optional<std::string> {
    if (textureName.empty()) {
        return std::nullopt;
    }

    std::string cleanName(textureName);
    if (cleanName.starts_with("materials/") || cleanName.starts_with("materials\\")) {
        cleanName = cleanName.substr(10);
    }
    if (cleanName.ends_with(".vtf") || cleanName.ends_with(".png") || cleanName.ends_with(".jpg")) {
        cleanName = cleanName.substr(0, cleanName.size() - 4);
    }

    // Candidates in priority: .vtf, .png, .jpg
    const std::array<std::string_view, 3> extensions {".vtf", ".png", ".jpg"};
    for (const auto& ext: extensions) {
        std::string cand = "materials/" + cleanName + std::string(ext);
        if (Exists(cand)) {
            return cand;
        }
        std::string candDirect = cleanName + std::string(ext);
        if (Exists(candDirect)) {
            return candDirect;
        }
    }

    return std::nullopt;
}

auto SourceVFS::ResolveModel(std::string_view modelName) const -> std::optional<std::string> {
    if (modelName.empty()) {
        return std::nullopt;
    }

    std::string cleanName(modelName);
    while (!cleanName.empty() && (cleanName.front() == '/' || cleanName.front() == '\\')) {
        cleanName.erase(cleanName.begin());
    }

    // Candidate 1: models/<name>.mdl
    std::string candidate = cleanName;
    if (!candidate.starts_with("models/") && !candidate.starts_with("models\\")) {
        candidate = "models/" + candidate;
    }
    if (!candidate.ends_with(".mdl")) {
        candidate += ".mdl";
    }
    if (Exists(candidate)) {
        return candidate;
    }

    // Candidate 2: direct path
    std::string candDirect = cleanName;
    if (!candDirect.ends_with(".mdl")) {
        candDirect += ".mdl";
    }
    if (Exists(candDirect)) {
        return candDirect;
    }

    return std::nullopt;
}

auto SourceVFS::ResolveLightmap(std::string_view mapName) const -> std::optional<std::string> {
    if (mapName.empty()) {
        return std::nullopt;
    }

    std::filesystem::path p(mapName);
    std::string baseName = p.stem().string();

    // Check lightmaps/<mapname>_lightmap0.png
    std::string cand1 = "lightmaps/" + baseName + "_lightmap0.png";
    if (Exists(cand1)) {
        return cand1;
    }

    // Check lightmaps/<mapname>_lightmap.png
    std::string cand2 = "lightmaps/" + baseName + "_lightmap.png";
    if (Exists(cand2)) {
        return cand2;
    }

    // Check materials/maps/<mapname>/lightmap0.png
    std::string cand3 = "materials/maps/" + baseName + "/lightmap0.png";
    if (Exists(cand3)) {
        return cand3;
    }

    return std::nullopt;
}

} // namespace ZHLN::BSP
