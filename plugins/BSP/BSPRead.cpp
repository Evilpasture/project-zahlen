// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// plugins/BSP/BSPRead.cpp
//
// Container parsing: the lump directory, the entity text lump, and the string
// table that gives every DTexData its material name. Struct payloads are read
// through the reflection-driven ReadStruct in BSPRead.hpp.

#include "BSPRead.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace ZHLN::BSP {
namespace {

void SkipWhitespaceAndComments(std::string_view& text) noexcept {
    while (!text.empty()) {
        if (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n') {
            text.remove_prefix(1);
            continue;
        }
        if (text.starts_with("//")) {
            const auto newline = text.find('\n');
            if (newline == std::string_view::npos) {
                text = {};
            } else {
                text.remove_prefix(newline + 1);
            }
            continue;
        }
        break;
    }
}

auto ReadQuotedString(std::string_view& text) noexcept -> std::optional<std::string_view> {
    if (text.empty() || text.front() != '"') {
        return std::nullopt;
    }
    text.remove_prefix(1);
    const auto closeQuote = text.find('"');
    if (closeQuote == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view result = text.substr(0, closeQuote);
    text.remove_prefix(closeQuote + 1);
    return result;
}

template <typename T>
auto ReadLumpArray(std::span<const std::byte> file, const LumpEntry& entry, std::vector<T>& out) -> std::expected<void, ErrorCode> {
    if (entry.filelen < 0 || entry.fileofs < 0 || static_cast<size_t>(entry.fileofs) + static_cast<size_t>(entry.filelen) > file.size()) {
        return std::unexpected(BSPError::LumpOutOfBounds);
    }
    if (entry.filelen == 0) {
        return {};
    }
    if (entry.filelen % static_cast<int32_t>(sizeof(T)) != 0) {
        return std::unexpected(BSPError::MalformedLump);
    }
    const size_t count = static_cast<size_t>(entry.filelen) / sizeof(T);
    out.resize(count);
    ByteReader in(file.subspan(static_cast<size_t>(entry.fileofs), static_cast<size_t>(entry.filelen)));
    for (T& element: out) {
        if (!ReadStruct(in, element)) {
            return std::unexpected(BSPError::ShortRead);
        }
    }
    return {};
}

// Texdata names live in a string table: LUMP_TEXDATA_STRING_TABLE is int32
// offsets into LUMP_TEXDATA_STRING_DATA (NUL-terminated).
auto ReadTexNames(
    std::span<const std::byte>   file,
    const LumpEntry&             dataEntry,
    const LumpEntry&             tableEntry,
    const std::vector<DTexData>& texDatas,
    std::vector<std::string>&    out
) -> std::expected<void, ErrorCode> {
    if (dataEntry.filelen < 0 || tableEntry.filelen < 0 || dataEntry.fileofs < 0 || tableEntry.fileofs < 0 ||
        static_cast<size_t>(dataEntry.fileofs) + static_cast<size_t>(dataEntry.filelen) > file.size() ||
        static_cast<size_t>(tableEntry.fileofs) + static_cast<size_t>(tableEntry.filelen) > file.size()) {
        return std::unexpected(BSPError::LumpOutOfBounds);
    }
    const auto dataSpan  = file.subspan(static_cast<size_t>(dataEntry.fileofs), static_cast<size_t>(dataEntry.filelen));
    const auto tableSpan = file.subspan(static_cast<size_t>(tableEntry.fileofs), static_cast<size_t>(tableEntry.filelen));

    out.clear();
    out.reserve(texDatas.size());
    for (const DTexData& texData: texDatas) {
        std::string name;
        // nameStringTableID indexes LUMP_TEXDATA_STRING_TABLE; each table entry
        // is a byte offset into LUMP_TEXDATA_STRING_DATA.
        const size_t tableIndex = (texData.nameStringTableID >= 0) ? static_cast<size_t>(texData.nameStringTableID) : 0;
        if (texData.nameStringTableID >= 0 && (tableIndex + 1) * sizeof(int32_t) <= tableSpan.size()) {
            int32_t offset = 0;
            std::memcpy(&offset, tableSpan.data() + tableIndex * sizeof(int32_t), sizeof(offset));
            if (offset >= 0 && static_cast<size_t>(offset) < dataSpan.size()) {
                const char*  str   = reinterpret_cast<const char*>(dataSpan.data()) + offset;
                const size_t limit = dataSpan.size() - static_cast<size_t>(offset);
                const size_t len   = std::min(std::strlen(str), limit);
                name.assign(str, len);
            }
        }
        if (name.empty()) {
            return std::unexpected(BSPError::UnresolvedTexData);
        }
        out.push_back(std::move(name));
    }
    return {};
}

} // namespace

auto BSPEntity::Find(std::string_view key) const noexcept -> std::string_view {
    for (const auto& [k, v]: keys) {
        if (k == key) {
            return v;
        }
    }
    return {};
}

auto BSPEntity::FindVector(std::string_view key, float out[3]) const noexcept -> bool {
    const std::string_view raw = Find(key);
    if (raw.empty()) {
        return false;
    }
    const std::string buffer(raw);
    const char*       cursor = buffer.c_str();
    char*             end    = nullptr;
    for (size_t k = 0; k < 3; ++k) {
        out[k] = std::strtof(cursor, &end);
        if (end == cursor) {
            return false;
        }
        cursor = end;
    }
    return true;
}

auto ParseEntities(std::string_view text) -> std::vector<BSPEntity> {
    std::vector<BSPEntity> entities;

    while (true) {
        SkipWhitespaceAndComments(text);
        if (text.empty() || text.front() != '{') {
            break;
        }
        text.remove_prefix(1);

        BSPEntity entity;
        while (true) {
            SkipWhitespaceAndComments(text);
            if (text.empty()) {
                break;
            }
            if (text.front() == '}') {
                text.remove_prefix(1);
                break;
            }

            const auto key = ReadQuotedString(text);
            if (!key) {
                break;
            }
            SkipWhitespaceAndComments(text);
            const auto val = ReadQuotedString(text);
            if (!val) {
                break;
            }
            entity.keys.emplace_back(std::string(*key), std::string(*val));
        }
        entities.push_back(std::move(entity));
    }
    return entities;
}

auto ParseBsp(std::span<const std::byte> bytes) -> std::expected<BSPMap, ErrorCode> {
    ByteReader file(bytes);
    BspHeader  header {};
    if (!ReadStruct(file, header)) {
        return std::unexpected(BSPError::FileTooSmall);
    }
    if (header.ident != kBspIdent) {
        return std::unexpected(BSPError::InvalidIdent);
    }
    if (header.version < kBspVersionMin || header.version > kBspVersionMax) {
        return std::unexpected(BSPError::UnsupportedVersion);
    }

    BSPMap map;
    map.version = header.version;

    const auto lump = [&](Lump id) -> const LumpEntry& { return header.lumps[static_cast<uint32_t>(id)]; };

    // The entity lump is text; everything else is struct arrays.
    const LumpEntry& entitiesLump = lump(Lump::Entities);
    if (entitiesLump.filelen > 0) {
        if (entitiesLump.fileofs < 0 || static_cast<size_t>(entitiesLump.fileofs) + static_cast<size_t>(entitiesLump.filelen) > bytes.size()) {
            return std::unexpected(BSPError::LumpOutOfBounds);
        }
        const auto textSpan = bytes.subspan(static_cast<size_t>(entitiesLump.fileofs), static_cast<size_t>(entitiesLump.filelen));
        map.entities        = ParseEntities(std::string_view(reinterpret_cast<const char*>(textSpan.data()), textSpan.size()));
    }

    if (auto res = ReadLumpArray(bytes, lump(Lump::Planes), map.planes); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = ReadLumpArray(bytes, lump(Lump::Vertexes), map.vertices); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = ReadLumpArray(bytes, lump(Lump::Edges), map.edges); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = ReadLumpArray(bytes, lump(Lump::SurfEdges), map.surfEdges); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = ReadLumpArray(bytes, lump(Lump::Faces), map.faces); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = ReadLumpArray(bytes, lump(Lump::TexInfo), map.texInfos); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = ReadLumpArray(bytes, lump(Lump::TexData), map.texDatas); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = ReadLumpArray(bytes, lump(Lump::Models), map.models); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = ReadLumpArray(bytes, lump(Lump::DispInfo), map.dispInfos); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = ReadLumpArray(bytes, lump(Lump::DispVerts), map.dispVerts); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = ReadLumpArray(bytes, lump(Lump::Nodes), map.nodes); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = ReadLumpArray(bytes, lump(Lump::Brushes), map.brushes); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = ReadLumpArray(bytes, lump(Lump::BrushSides), map.brushSides); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = ReadLumpArray(bytes, lump(Lump::Overlays), map.overlays); !res) {
        return std::unexpected(res.error());
    }

    // Leaves: version 0 is 30-byte DLeaf, version 1 appends two padding bytes.
    const LumpEntry& leafLump = lump(Lump::Leafs);
    if (leafLump.filelen > 0) {
        const size_t stride = (leafLump.version == 1) ? sizeof(DLeaf) + 2 : sizeof(DLeaf);
        if (leafLump.fileofs < 0 || leafLump.filelen < 0 || static_cast<size_t>(leafLump.fileofs) + static_cast<size_t>(leafLump.filelen) > bytes.size()) {
            return std::unexpected(BSPError::LumpOutOfBounds);
        }
        if (static_cast<size_t>(leafLump.filelen) % stride != 0) {
            return std::unexpected(BSPError::MalformedLump);
        }
        ByteReader   in(bytes.subspan(static_cast<size_t>(leafLump.fileofs), static_cast<size_t>(leafLump.filelen)));
        const size_t count = static_cast<size_t>(leafLump.filelen) / stride;
        map.leafs.resize(count);
        for (DLeaf& leaf: map.leafs) {
            if (!ReadStruct(in, leaf) || !in.Skip(stride - sizeof(DLeaf))) {
                return std::unexpected(BSPError::ShortRead);
            }
        }
    }

    if (auto res = ReadTexNames(bytes, lump(Lump::TexDataStringData), lump(Lump::TexDataStringTable), map.texDatas, map.texNames); !res) {
        return std::unexpected(res.error());
    }

    return map;
}

} // namespace ZHLN::BSP
