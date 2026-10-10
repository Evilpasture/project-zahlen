// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// plugins/BSP/BSPRead.cpp
//
// Container parsing: the lump directory, the entity text lump, and the string
// table that gives every DTexData its material name. Struct payloads are read
// through the reflection-driven ReadStruct in BSPRead.hpp.

#include "BSPRead.hpp"

#include <algorithm>
#include <cstring>

namespace ZHLN::BSP {
namespace {

auto IsEntitySpace(char c) -> bool {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Reads one quoted string starting at *cursor (which must be on the opening
// quote). Returns false on end-of-text before the closing quote.
auto ReadQuoted(std::string_view text, size_t& cursor, std::string& out) -> bool {
    if (cursor >= text.size() || text[cursor] != '"') {
        return false;
    }
    ++cursor;
    out.clear();
    while (cursor < text.size() && text[cursor] != '"') {
        out.push_back(text[cursor]);
        ++cursor;
    }
    if (cursor >= text.size()) {
        return false;
    }
    ++cursor; // closing quote
    return true;
}

void SkipLineComments(std::string_view text, size_t& cursor) {
    while (cursor + 1 < text.size() && text[cursor] == '/' && text[cursor + 1] == '/') {
        while (cursor < text.size() && text[cursor] != '\n') {
            ++cursor;
        }
    }
}

template <typename T>
auto ReadLumpArray(std::span<const std::byte> file, const LumpEntry& entry, std::vector<T>& out, const char* name, std::string& error) -> bool {
    if (entry.filelen < 0 || entry.fileofs < 0 || static_cast<size_t>(entry.fileofs) + static_cast<size_t>(entry.filelen) > file.size()) {
        error = std::string("lump out of bounds: ") + name;
        return false;
    }
    if (entry.filelen == 0) {
        return true;
    }
    if (entry.filelen % static_cast<int32_t>(sizeof(T)) != 0) {
        error = std::string("lump size is not a multiple of the struct: ") + name;
        return false;
    }
    const size_t count = static_cast<size_t>(entry.filelen) / sizeof(T);
    out.resize(count);
    ByteReader in(file.subspan(static_cast<size_t>(entry.fileofs), static_cast<size_t>(entry.filelen)));
    for (T& element: out) {
        if (!ReadStruct(in, element)) {
            error = std::string("short read in lump: ") + name;
            return false;
        }
    }
    return true;
}

// Texdata names live in a string table: LUMP_TEXDATA_STRING_TABLE is int32
// offsets into LUMP_TEXDATA_STRING_DATA (NUL-terminated).
auto ReadTexNames(std::span<const std::byte> file, const LumpEntry& dataEntry, const LumpEntry& tableEntry, const std::vector<DTexData>& texDatas, std::vector<std::string>& out, std::string& error) -> bool {
    if (dataEntry.filelen < 0 || tableEntry.filelen < 0) {
        error = "texdata string lumps out of bounds";
        return false;
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
                const char* str    = reinterpret_cast<const char*>(dataSpan.data()) + offset;
                const size_t limit = dataSpan.size() - static_cast<size_t>(offset);
                const size_t len   = std::min(std::strlen(str), limit);
                name.assign(str, len);
            }
        }
        if (name.empty()) {
            error = "unresolved texdata name";
            return false;
        }
        out.push_back(std::move(name));
    }
    return true;
}

} // namespace

auto ParseEntities(std::string_view text) -> std::vector<BSPEntity> {
    std::vector<BSPEntity> entities;
    size_t                 cursor = 0;
    std::string            scratch;

    while (cursor < text.size()) {
        SkipLineComments(text, cursor);
        while (cursor < text.size() && IsEntitySpace(text[cursor])) {
            ++cursor;
        }
        SkipLineComments(text, cursor);
        if (cursor >= text.size() || text[cursor] != '{') {
            break;
        }
        ++cursor;

        BSPEntity entity;
        while (cursor < text.size()) {
            SkipLineComments(text, cursor);
            while (cursor < text.size() && IsEntitySpace(text[cursor])) {
                ++cursor;
            }
            SkipLineComments(text, cursor);
            if (cursor >= text.size()) {
                break;
            }
            if (text[cursor] == '}') {
                ++cursor;
                break;
            }
            std::string key;
            std::string value;
            if (!ReadQuoted(text, cursor, key)) {
                break;
            }
            while (cursor < text.size() && IsEntitySpace(text[cursor])) {
                ++cursor;
            }
            if (!ReadQuoted(text, cursor, value)) {
                break;
            }
            entity.keys.emplace_back(std::move(key), std::move(value));
        }
        entities.push_back(std::move(entity));
    }
    return entities;
}

auto ParseBsp(std::span<const std::byte> bytes) -> ParseResult {
    ParseResult result;

    ByteReader file(bytes);
    BspHeader  header {};
    if (!ReadStruct(file, header)) {
        result.error = "file too small for a BSP header";
        return result;
    }
    if (header.ident != kBspIdent) {
        result.error = "not a VBSP image";
        return result;
    }
    if (header.version < kBspVersionMin || header.version > kBspVersionMax) {
        result.error = "unsupported BSP version (mainline 19-21 only): " + std::to_string(header.version);
        return result;
    }
    result.map.version = header.version;

    const auto lump = [&](Lump id) -> const LumpEntry& { return header.lumps[static_cast<uint32_t>(id)]; };

    // The entity lump is text; everything else is struct arrays.
    const LumpEntry& entitiesLump = lump(Lump::Entities);
    if (entitiesLump.filelen > 0) {
        if (entitiesLump.fileofs < 0 || static_cast<size_t>(entitiesLump.fileofs) + static_cast<size_t>(entitiesLump.filelen) > bytes.size()) {
            result.error = "entity lump out of bounds";
            return result;
        }
        const auto textSpan = bytes.subspan(static_cast<size_t>(entitiesLump.fileofs), static_cast<size_t>(entitiesLump.filelen));
        result.map.entities = ParseEntities(std::string_view(reinterpret_cast<const char*>(textSpan.data()), textSpan.size()));
    }

    bool ok = true;
    ok = ok && ReadLumpArray(bytes, lump(Lump::Planes), result.map.planes, "planes", result.error);
    ok = ok && ReadLumpArray(bytes, lump(Lump::Vertexes), result.map.vertices, "vertexes", result.error);
    ok = ok && ReadLumpArray(bytes, lump(Lump::Edges), result.map.edges, "edges", result.error);
    ok = ok && ReadLumpArray(bytes, lump(Lump::SurfEdges), result.map.surfEdges, "surfedges", result.error);
    ok = ok && ReadLumpArray(bytes, lump(Lump::Faces), result.map.faces, "faces", result.error);
    ok = ok && ReadLumpArray(bytes, lump(Lump::TexInfo), result.map.texInfos, "texinfo", result.error);
    ok = ok && ReadLumpArray(bytes, lump(Lump::TexData), result.map.texDatas, "texdata", result.error);
    ok = ok && ReadLumpArray(bytes, lump(Lump::Models), result.map.models, "models", result.error);
    ok = ok && ReadLumpArray(bytes, lump(Lump::DispInfo), result.map.dispInfos, "dispinfo", result.error);
    ok = ok && ReadLumpArray(bytes, lump(Lump::DispVerts), result.map.dispVerts, "dispverts", result.error);
    ok = ok && ReadLumpArray(bytes, lump(Lump::Nodes), result.map.nodes, "nodes", result.error);
    ok = ok && ReadLumpArray(bytes, lump(Lump::Brushes), result.map.brushes, "brushes", result.error);
    ok = ok && ReadLumpArray(bytes, lump(Lump::BrushSides), result.map.brushSides, "brushsides", result.error);
    ok = ok && ReadLumpArray(bytes, lump(Lump::Overlays), result.map.overlays, "overlays", result.error);
    if (!ok) {
        return result;
    }

    // Leaves: version 0 is 30-byte DLeaf, version 1 appends two padding bytes.
    {
        const LumpEntry& leafLump = lump(Lump::Leafs);
        if (leafLump.filelen > 0) {
            const size_t stride = (leafLump.version == 1) ? sizeof(DLeaf) + 2 : sizeof(DLeaf);
            if (leafLump.fileofs < 0 || leafLump.filelen < 0 ||
                static_cast<size_t>(leafLump.fileofs) + static_cast<size_t>(leafLump.filelen) > bytes.size() ||
                static_cast<size_t>(leafLump.filelen) % stride != 0) {
                result.error = "leaf lump malformed";
                return result;
            }
            ByteReader in(bytes.subspan(static_cast<size_t>(leafLump.fileofs), static_cast<size_t>(leafLump.filelen)));
            const size_t count = static_cast<size_t>(leafLump.filelen) / stride;
            result.map.leafs.resize(count);
            for (DLeaf& leaf: result.map.leafs) {
                if (!ReadStruct(in, leaf) || !in.Skip(stride - sizeof(DLeaf))) {
                    result.error = "short read in lump: leafs";
                    return result;
                }
            }
        }
    }

    if (!ReadTexNames(bytes, lump(Lump::TexDataStringData), lump(Lump::TexDataStringTable), result.map.texDatas, result.map.texNames, result.error)) {
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ZHLN::BSP
