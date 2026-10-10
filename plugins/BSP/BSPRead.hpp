// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// plugins/BSP/BSPRead.hpp
//
// Bounds-checked lump reading for the mainline BSP format. The structural
// reading is reflection-driven: ZHLN::Reflect::ForEachField walks a packed
// on-disk aggregate once per type, so there is exactly one reader for every
// struct in BSPTypes.hpp and no per-lump hand-written read loop to keep in
// sync with the file order.
//
// Everything here is engine-free (std + Core/Reflection only): it is the part
// that can be built and validated without the rest of the tree.

#include "BSPTypes.hpp"
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <cstddef>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::BSP {

enum class BSPError : uint8_t {
    FileTooSmall       ZHLN_ANNOTATION(ZHLN::Description<"File too small for a BSP header"> {}) = 1,
    InvalidIdent       ZHLN_ANNOTATION(ZHLN::Description<"Not a valid VBSP image (bad magic)"> {}),
    UnsupportedVersion ZHLN_ANNOTATION(ZHLN::Description<"Unsupported BSP version (mainline 19-21 only)"> {}),
    LumpOutOfBounds    ZHLN_ANNOTATION(ZHLN::Description<"BSP lump directory entry is out of bounds"> {}),
    MalformedLump      ZHLN_ANNOTATION(ZHLN::Description<"BSP lump size is malformed or unaligned"> {}),
    ShortRead          ZHLN_ANNOTATION(ZHLN::Description<"Unexpected end of stream while reading BSP lump"> {}),
    UnresolvedTexData  ZHLN_ANNOTATION(ZHLN::Description<"Unresolved texdata material name"> {}),
};

// A forward cursor over one lump (or the whole file) that never reads past the
// end: a short read fails the cursor and any ReadStruct built on it, rather
// than trusting the lump directory's lengths.
class ByteReader {
  public:
    constexpr ByteReader() = default;
    constexpr explicit ByteReader(std::span<const std::byte> bytes): _bytes(bytes) {
    }

    template <typename T>
    auto Read(T& out) noexcept -> bool {
        static_assert(std::is_trivially_copyable_v<T>);
        if (_cursor + sizeof(T) > _bytes.size()) {
            return false;
        }
        std::memcpy(&out, _bytes.data() + _cursor, sizeof(T));
        _cursor += sizeof(T);
        return true;
    }

    auto Skip(size_t count) noexcept -> bool {
        if (_cursor + count > _bytes.size()) {
            return false;
        }
        _cursor += count;
        return true;
    }

    [[nodiscard]] auto Remaining() const noexcept -> size_t {
        return _bytes.size() - _cursor;
    }
    [[nodiscard]] auto Cursor() const noexcept -> size_t {
        return _cursor;
    }

  private:
    std::span<const std::byte> _bytes {};
    size_t                     _cursor = 0;
};

// Field-wise read of one packed on-disk struct (or one scalar element, as the
// surfedge lump is an array of int32). Returns false on a short read.
template <typename T>
auto ReadStruct(ByteReader& in, T& out) -> bool {
    using U = std::remove_cvref_t<T>;
    static_assert(std::is_trivially_copyable_v<U>, "on-disk data is trivially copyable");
    if constexpr (std::is_aggregate_v<U>) {
        bool ok = true;
        Reflect::ForEachField(out, [&](auto& member) {
            if (ok) {
                ok = in.Read(member);
            }
        });
        if constexpr (Reflect::FieldCount<U>() == 0) {
            return in.Read(out);
        }
        return ok;
    } else {
        return in.Read(out);
    }
}

// One entity from LUMP_ENTITIES: an ordered key/value list, Source style.
struct BSPEntity {
    std::vector<std::pair<std::string, std::string>> keys;

    [[nodiscard]] auto Find(std::string_view key) const noexcept -> std::string_view;
    [[nodiscard]] auto Has(std::string_view key) const noexcept -> bool {
        return !Find(key).empty();
    }

    // "x y z" (or "p y r") keyvalues, Source convention. Returns false when the
    // key is missing or does not parse to exactly three floats.
    [[nodiscard]] auto FindVector(std::string_view key, float out[3]) const noexcept -> bool;
};

// The parsed map: every lump the importer needs, plus the entity list and
// resolved material names. Raw lump spans stay valid only while the caller's
// file bytes live; the struct vectors are owned copies.
struct BSPMap {
    int32_t version = 0;

    std::vector<BSPEntity> entities;

    std::vector<DPlane>      planes;
    std::vector<DVertex>     vertices;
    std::vector<DEdge>       edges;
    std::vector<int32_t>     surfEdges;
    std::vector<DFace>       faces;
    std::vector<DTexInfo>    texInfos;
    std::vector<DTexData>    texDatas;
    std::vector<std::string> texNames; // parallel to texDatas
    std::vector<DModel>      models;
    std::vector<DDispInfo>   dispInfos;
    std::vector<DDispVert>   dispVerts;
    std::vector<DNode>       nodes;
    std::vector<DLeaf>       leafs;
    std::vector<DBrush>      brushes;
    std::vector<DBrushSide>  brushSides;
    std::vector<DOverlay>    overlays;
};

// Parses a whole .bsp image. Strict on the container (ident, version window,
// lump bounds) and on every struct read; empty lumps are legal and common.
[[nodiscard]] auto ParseBsp(std::span<const std::byte> bytes) -> std::expected<BSPMap, ErrorCode>;

// LUMP_ENTITIES text -> entities.
[[nodiscard]] auto ParseEntities(std::string_view text) -> std::vector<BSPEntity>;

} // namespace ZHLN::BSP
