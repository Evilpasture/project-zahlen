// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// plugins/BSP/BSPRead.cpp
//
// Container parsing: the lump directory, the entity text lump, and the string
// table that gives every DTexData its material name. Struct payloads are read
// through the reflection-driven ReadStruct in BSPRead.hpp.

#include "BSPRead.hpp"
#include <algorithm>
#include <charconv>
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

auto BSPEntity::FindVector(std::string_view key) const noexcept -> std::optional<std::array<float, 3>> {
    const std::string_view raw = Find(key);
    if (raw.empty()) {
        return std::nullopt;
    }
    const char*          cur = raw.data();
    const char*          end = raw.data() + raw.size();
    std::array<float, 3> result {};
    for (size_t k = 0; k < 3; ++k) {
        while (cur < end && (*cur == ' ' || *cur == '\t')) {
            cur++;
        }
        if (cur >= end) {
            return std::nullopt;
        }
        auto [ptr, ec] = std::from_chars(cur, end, result[k]);
        if (ec != std::errc {}) {
            return std::nullopt;
        }
        cur = ptr;
    }
    return result;
}

auto BSPEntity::FindVector(std::string_view key, float out[3]) const noexcept -> bool {
    const auto vec = FindVector(key);
    if (!vec) {
        return false;
    }
    out[0] = (*vec)[0];
    out[1] = (*vec)[1];
    out[2] = (*vec)[2];
    return true;
}

auto BSPEntity::FindFloat(std::string_view key) const noexcept -> std::optional<float> {
    const std::string_view raw = Find(key);
    if (raw.empty()) {
        return std::nullopt;
    }
    const char* cur = raw.data();
    const char* end = raw.data() + raw.size();
    while (cur < end && (*cur == ' ' || *cur == '\t')) {
        cur++;
    }
    if (cur >= end) {
        return std::nullopt;
    }
    float val = 0.0f;
    if (auto [ptr, ec] = std::from_chars(cur, end, val); ec == std::errc {}) {
        return val;
    }
    return std::nullopt;
}

auto BSPEntity::FindInt(std::string_view key) const noexcept -> std::optional<int32_t> {
    const std::string_view raw = Find(key);
    if (raw.empty()) {
        return std::nullopt;
    }
    const char* cur = raw.data();
    const char* end = raw.data() + raw.size();
    while (cur < end && (*cur == ' ' || *cur == '\t')) {
        cur++;
    }
    if (cur >= end) {
        return std::nullopt;
    }
    int32_t val = 0;
    if (auto [ptr, ec] = std::from_chars(cur, end, val); ec == std::errc {}) {
        return val;
    }
    return std::nullopt;
}

auto ParseGameLump(std::span<const std::byte> file, const LumpEntry& gameLumpEntry, BSPMap& map)
    -> std::expected<void, ErrorCode> {
    if (gameLumpEntry.filelen == 0) {
        return {};
    }
    if (gameLumpEntry.fileofs < 0 || gameLumpEntry.filelen < 0 ||
        static_cast<size_t>(gameLumpEntry.fileofs) + static_cast<size_t>(gameLumpEntry.filelen) > file.size()) {
        return std::unexpected(BSPError::LumpOutOfBounds);
    }
    if (gameLumpEntry.filelen < static_cast<int32_t>(sizeof(int32_t))) {
        return {};
    }

    const auto gameLumpSpan = file.subspan(static_cast<size_t>(gameLumpEntry.fileofs), static_cast<size_t>(gameLumpEntry.filelen));
    ByteReader reader(gameLumpSpan);

    int32_t lumpCount = 0;
    if (!reader.Read(lumpCount) || lumpCount < 0) {
        return std::unexpected(BSPError::MalformedLump);
    }

    map.gameLumps.reserve(static_cast<size_t>(lumpCount));
    const DGameLump* sprpLump = nullptr;

    for (int32_t i = 0; i < lumpCount; ++i) {
        DGameLump gl {};
        if (!reader.Read(gl)) {
            return std::unexpected(BSPError::ShortRead);
        }
        map.gameLumps.push_back(gl);
        if (gl.id == kGameLumpStaticProps || gl.id == kGameLumpStaticPropsAlt) {
            sprpLump = &map.gameLumps.back();
        }
    }

    if (!sprpLump || sprpLump->filelen <= 0) {
        return {};
    }

    size_t sprpOffset = static_cast<size_t>(sprpLump->fileofs);
    const size_t sprpLen = static_cast<size_t>(sprpLump->filelen);
    if (sprpLump->fileofs < 0 || sprpOffset + sprpLen > file.size()) {
        if (static_cast<size_t>(gameLumpEntry.fileofs) + sprpOffset + sprpLen <= file.size()) {
            sprpOffset += static_cast<size_t>(gameLumpEntry.fileofs);
        } else {
            return std::unexpected(BSPError::LumpOutOfBounds);
        }
    }

    if ((sprpLump->flags & 1) != 0) {
        return {};
    }

    const auto sprpSpan = file.subspan(sprpOffset, sprpLen);
    ByteReader propReader(sprpSpan);

    // 1. Model Dictionary
    int32_t dictEntries = 0;
    if (!propReader.Read(dictEntries) || dictEntries < 0) {
        return {};
    }
    std::vector<std::string> modelDict;
    modelDict.reserve(static_cast<size_t>(dictEntries));
    for (int32_t i = 0; i < dictEntries; ++i) {
        char nameBuf[128] {};
        if (!propReader.Read(nameBuf)) {
            return {};
        }
        nameBuf[127] = '\0';
        modelDict.emplace_back(nameBuf);
    }

    // 2. Leaf Array
    int32_t leafEntries = 0;
    if (!propReader.Read(leafEntries) || leafEntries < 0) {
        return {};
    }
    if (!propReader.Skip(static_cast<size_t>(leafEntries) * sizeof(uint16_t))) {
        return {};
    }

    // 3. Static Props
    int32_t propCount = 0;
    if (!propReader.Read(propCount) || propCount < 0) {
        return {};
    }

    if (propCount == 0) {
        return {};
    }

    const uint16_t version = sprpLump->version;
    size_t stride = 56;
    if ((propReader.Remaining() % static_cast<size_t>(propCount)) == 0 &&
        (propReader.Remaining() / static_cast<size_t>(propCount)) >= 56) {
        stride = propReader.Remaining() / static_cast<size_t>(propCount);
    } else {
        switch (version) {
            case 4:  stride = 56; break;
            case 5:  stride = 60; break;
            case 6:  stride = 64; break;
            case 7:  stride = 68; break;
            case 8:  stride = 68; break;
            case 9:  stride = 72; break;
            case 10: stride = 76; break;
            case 11: stride = 80; break;
            default: stride = (version >= 11) ? 80 : 56; break;
        }
    }

    if (stride < 56 || propReader.Remaining() < static_cast<size_t>(propCount) * stride) {
        return std::unexpected(BSPError::ShortRead);
    }

    map.staticProps.reserve(static_cast<size_t>(propCount));
    for (int32_t i = 0; i < propCount; ++i) {
        const size_t propStart = propReader.Cursor();
        StaticProp prop;

        float origin[3] {};
        float angles[3] {};
        float lightingOrigin[3] {};

        if (!propReader.Read(origin) ||
            !propReader.Read(angles) ||
            !propReader.Read(prop.propType) ||
            !propReader.Read(prop.firstLeaf) ||
            !propReader.Read(prop.leafCount) ||
            !propReader.Read(prop.solid) ||
            !propReader.Read(prop.flags) ||
            !propReader.Read(prop.skin) ||
            !propReader.Read(prop.fadeMinDist) ||
            !propReader.Read(prop.fadeMaxDist) ||
            !propReader.Read(lightingOrigin)) {
            return std::unexpected(BSPError::ShortRead);
        }

        prop.origin         = {origin[0], origin[1], origin[2]};
        prop.angles         = {angles[0], angles[1], angles[2]};
        prop.lightingOrigin = {lightingOrigin[0], lightingOrigin[1], lightingOrigin[2]};

        if (prop.propType < modelDict.size()) {
            prop.modelName = modelDict[prop.propType];
        }

        if (version >= 5 && stride >= 60) {
            float fadeScale = 1.0f;
            std::memcpy(&fadeScale, sprpSpan.data() + propStart + 56, sizeof(float));
            prop.forcedFadeScale = fadeScale;
        }

        if (version >= 7 && stride >= 68) {
            uint8_t rgba[4] {};
            std::memcpy(rgba, sprpSpan.data() + propStart + 64, 4);
            if (rgba[0] != 0 || rgba[1] != 0 || rgba[2] != 0 || rgba[3] != 0) {
                std::memcpy(prop.diffuseModulation.data(), rgba, 4);
            }
        }

        if (version >= 11 && stride >= 76) {
            float uScale = 1.0f;
            std::memcpy(&uScale, sprpSpan.data() + propStart + stride - sizeof(float), sizeof(float));
            if (uScale > 0.0001f && uScale < 10000.0f) {
                prop.uniformScale = uScale;
            }
        }

        map.staticProps.push_back(std::move(prop));
        propReader.Skip(stride - (propReader.Cursor() - propStart));
    }

    return {};
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

    const LumpEntry* facesLump = &lump(Lump::Faces);
    if (lump(Lump::FacesHDR).filelen > 0) {
        facesLump = &lump(Lump::FacesHDR);
    }
    if (auto res = ReadLumpArray(bytes, *facesLump, map.faces); !res) {
        if (facesLump != &lump(Lump::Faces)) {
            if (auto fallbackRes = ReadLumpArray(bytes, lump(Lump::Faces), map.faces); !fallbackRes) {
                return std::unexpected(res.error());
            }
        } else {
            return std::unexpected(res.error());
        }
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

    // Leaves: support version 0 (30 bytes or 32/56 with padding/ambient) and version 1 (32 bytes).
    const LumpEntry& leafLump = lump(Lump::Leafs);
    if (leafLump.filelen > 0) {
        if (leafLump.fileofs < 0 || leafLump.filelen < 0 || static_cast<size_t>(leafLump.fileofs) + static_cast<size_t>(leafLump.filelen) > bytes.size()) {
            return std::unexpected(BSPError::LumpOutOfBounds);
        }

        size_t stride = 0;
        if (leafLump.version == 1 && (leafLump.filelen % (sizeof(DLeaf) + 2)) == 0) {
            stride = sizeof(DLeaf) + 2; // 32
        } else if (leafLump.filelen % sizeof(DLeaf) == 0) {
            stride = sizeof(DLeaf); // 30
        } else if (leafLump.filelen % 32 == 0) {
            stride = 32;
        } else if (leafLump.filelen % 56 == 0) {
            stride = 56;
        }

        if (stride == 0) {
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

    if (auto res = ParseGameLump(bytes, lump(Lump::GameLump), map); !res) {
        return std::unexpected(res.error());
    }

    return map;
}

} // namespace ZHLN::BSP
