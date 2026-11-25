// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SourceVFS.hpp"
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/FileSystem/VFS.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

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

    // Collapse multiple consecutive slashes and trim trailing slashes
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
    while (!collapsed.empty() && collapsed.back() == '/') {
        collapsed.pop_back();
    }
    return collapsed;
}

void SourceVFS::SetVirtualPrefix(std::string_view virtualPrefix) {
    _virtualPrefix = NormalizePath(virtualPrefix);
}

auto SourceVFS::ApplyPrefix(std::string_view normalizedPath) const -> std::string {
    if (_virtualPrefix.empty()) {
        return std::string(normalizedPath);
    }
    if (normalizedPath.empty()) {
        return _virtualPrefix;
    }
    std::string out;
    out.reserve(_virtualPrefix.size() + 1 + normalizedPath.size());
    out.append(_virtualPrefix);
    out.push_back('/');
    out.append(normalizedPath);
    return out;
}

auto SourceVFS::BackingExists(std::string_view candidatePath) const -> bool {
    if (_assets != nullptr && _assets->VirtualFileExists(candidatePath)) {
        return true;
    }
    if (_vfs != nullptr && _vfs->Exists(candidatePath)) {
        return true;
    }
    return false;
}

auto SourceVFS::BackingRead(std::string_view candidatePath) const -> std::optional<std::vector<std::byte>> {
    if (_assets != nullptr) {
        if (auto file = _assets->OpenVirtualFile(candidatePath)) {
            const auto span = (*file)->Bytes();
            return std::vector<std::byte>(span.data, span.data + span.size);
        }
    }
    if (_vfs != nullptr) {
        if (auto file = _vfs->Open(candidatePath)) {
            const auto span = file->Bytes();
            return std::vector<std::byte>(span.begin(), span.end());
        }
    }
    return std::nullopt;
}

void SourceVFS::IndexPakfile(std::span<const std::byte> pakData) {
    if (pakData.size() < 22) {
        return;
    }

    _pakDataCopy.assign(pakData.begin(), pakData.end());

    // Search for ZIP End of Central Directory (EOCD) signature: 0x06054b50
    const uint8_t* bytes    = reinterpret_cast<const uint8_t*>(_pakDataCopy.data());
    const size_t   totalLen = _pakDataCopy.size();

    int64_t      eocdPos   = -1;
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

    const uint8_t* eocd     = bytes + eocdPos;
    uint32_t       cdOffset = 0;
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

        std::string       fileName(reinterpret_cast<const char*>(cur + 46), nameLen);
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
    _pakDataCopy.clear();
    IndexPakfile(pakData);
}

auto SourceVFS::ResolveFile(std::string_view virtualPath) const -> std::optional<std::string> {
    if (virtualPath.empty() || virtualPath.front() == '/' || virtualPath.front() == '\\') {
        return std::nullopt;
    }

    const std::string norm = NormalizePath(virtualPath);
    if (norm.empty()) {
        return std::nullopt;
    }

    if (_pakIndex.contains(norm)) {
        return norm;
    }

    const std::string prefixed = ApplyPrefix(norm);
    if (BackingExists(prefixed)) {
        return norm;
    }

    if (!_virtualPrefix.empty() && norm.size() > _virtualPrefix.size() + 1 &&
        norm.starts_with(_virtualPrefix) && norm[_virtualPrefix.size()] == '/' &&
        BackingExists(norm)) {
        return norm;
    }

    return std::nullopt;
}

auto SourceVFS::Exists(std::string_view virtualPath) const -> bool {
    return ResolveFile(virtualPath).has_value();
}

auto SourceVFS::ReadFile(std::string_view virtualPath) const -> std::optional<std::vector<std::byte>> {
    if (virtualPath.empty() || virtualPath.front() == '/' || virtualPath.front() == '\\') {
        return std::nullopt;
    }

    const std::string norm = NormalizePath(virtualPath);
    if (norm.empty()) {
        return std::nullopt;
    }

    // 1. Check embedded pakfile first (map Lump 40 always overrides loose/mounted game assets).
    if (auto it = _pakIndex.find(norm); it != _pakIndex.end()) {
        const auto& entry = it->second;
        if (entry.offset + entry.compSize <= _pakDataCopy.size()) {
            if (entry.method == 0) { // Store (uncompressed)
                std::vector<std::byte> result(entry.uncompSize);
                if (entry.uncompSize > 0) {
                    std::memcpy(result.data(), _pakDataCopy.data() + entry.offset, entry.uncompSize);
                }
                return result;
            }
        }
    }

    // 2. Read from the backing VFS under `_virtualPrefix` (if any).
    const std::string prefixed = ApplyPrefix(norm);
    if (auto bytes = BackingRead(prefixed)) {
        return bytes;
    }

    if (!_virtualPrefix.empty() && norm.size() > _virtualPrefix.size() + 1 &&
        norm.starts_with(_virtualPrefix) && norm[_virtualPrefix.size()] == '/') {
        if (auto bytes = BackingRead(norm)) {
            return bytes;
        }
    }

    return std::nullopt;
}

auto SourceVFS::ResolveMaterial(std::string_view materialName) const -> std::optional<std::string> {
    if (materialName.empty()) {
        return std::nullopt;
    }

    std::string cleanName = NormalizePath(materialName);
    if (cleanName.empty()) {
        return std::nullopt;
    }
    if (cleanName.starts_with("materials/")) {
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

    std::string cleanName = NormalizePath(textureName);
    if (cleanName.empty()) {
        return std::nullopt;
    }
    if (cleanName.starts_with("materials/")) {
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

    std::string cleanName = NormalizePath(modelName);
    if (cleanName.empty()) {
        return std::nullopt;
    }

    // Candidate 1: models/<name>.mdl
    std::string candidate = cleanName;
    if (!candidate.starts_with("models/")) {
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

} // namespace ZHLN::BSP
