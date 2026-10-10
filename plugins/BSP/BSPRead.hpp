// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// plugins/BSP/BSPRead.hpp
//
// Bounds-checked lump reading for the mainline BSP format. The structural
// reading is reflection-driven: ZHLN::Reflect::ForEachField walks a packed
// on-disk aggregate once per type, so there is exactly one reader and one
// writer for every struct in BSPTypes.hpp and no per-lump read()/write() to
// keep in sync with the file order. That generic path is what the standalone
// calculator round-trips against the raw bytes.
//
// Everything here is engine-free (std + Core/Reflection only): it is the part
// that can be built and validated without the rest of the tree.

#include "BSPTypes.hpp"

#include <Zahlen/Core/Reflection/Structs.hpp>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::BSP {

// A forward cursor over one lump (or the whole file) that never reads past the
// end: a short read fails the cursor and any ReadStruct built on it, rather
// than trusting the lump directory's lengths.
class ByteReader {
public:
    constexpr ByteReader() = default;
    constexpr explicit ByteReader(std::span<const std::byte> bytes) : m_bytes(bytes) {}

    [[nodiscard]] constexpr auto Remaining() const noexcept -> size_t { return m_bytes.size() - m_cursor; }
    [[nodiscard]] constexpr auto Failed() const noexcept -> bool { return m_failed; }
    [[nodiscard]] constexpr auto Cursor() const noexcept -> size_t { return m_cursor; }

    auto ReadBytes(void* dst, size_t count) -> bool {
        if (m_failed || count > Remaining()) {
            m_failed = true;
            return false;
        }
        std::memcpy(dst, m_bytes.data() + m_cursor, count);
        m_cursor += count;
        return true;
    }

    auto Skip(size_t count) -> bool {
        if (m_failed || count > Remaining()) {
            m_failed = true;
            return false;
        }
        m_cursor += count;
        return true;
    }

    [[nodiscard]] auto SubReader(size_t count) -> std::optional<ByteReader> {
        if (m_failed || count > Remaining()) {
            m_failed = true;
            return std::nullopt;
        }
        ByteReader sub(m_bytes.subspan(m_cursor, count));
        m_cursor += count;
        return sub;
    }

    [[nodiscard]] auto Tail() const -> std::span<const std::byte> { return m_bytes.subspan(m_cursor); }

private:
    std::span<const std::byte> m_bytes {};
    size_t                     m_cursor = 0;
    bool                       m_failed = false;
};

namespace ReflectedReader {

// One member, read by shape: a trivially copyable member (including C arrays
// and nested C arrays) is its own contiguous run of file bytes; anything else
// is not a valid on-disk struct member and fails the read.
template <typename T>
auto ReadMember(ByteReader& in, T& member) -> bool {
    using M = std::remove_cvref_t<T>;
    static_assert(std::is_trivially_copyable_v<M>, "BSP on-disk structs must be trivially copyable");
    return in.ReadBytes(&member, sizeof(M));
}

template <typename T>
auto WriteMember(std::vector<std::byte>& out, const T& member) -> void {
    using M = std::remove_cvref_t<T>;
    static_assert(std::is_trivially_copyable_v<M>, "BSP on-disk structs must be trivially copyable");
    const auto* src = reinterpret_cast<const std::byte*>(&member);
    out.insert(out.end(), src, src + sizeof(M));
}

} // namespace ReflectedReader

// Field-wise read of one packed on-disk struct (or one scalar element, as the
// surfedge lump is an array of int32). Returns false on a short read. The body
// must work with or without native P2996 support: the Reflect API carries both
// spellings, and the transpiler handles the rest.
template <typename T>
auto ReadStruct(ByteReader& in, T& out) -> bool {
    using U = std::remove_cvref_t<T>;
    static_assert(std::is_trivially_copyable_v<U>, "on-disk data is trivially copyable");
    if constexpr (std::is_aggregate_v<U>) {
        bool ok = true;
        Reflect::ForEachField(out, [&](auto& member) {
            if (ok) {
                ok = ReflectedReader::ReadMember(in, member);
            }
        });
        return ok && !in.Failed();
    } else {
        return ReflectedReader::ReadMember(in, out) && !in.Failed();
    }
}

// Inverse of ReadStruct: byte-exact for any struct ReadStruct accepts. Used by
// the calculator's round-trip check and by nothing in the runtime path.
template <typename T>
auto WriteStruct(std::vector<std::byte>& out, const T& value) -> void {
    using U = std::remove_cvref_t<T>;
    static_assert(std::is_trivially_copyable_v<U>, "on-disk data is trivially copyable");
    if constexpr (std::is_aggregate_v<U>) {
        Reflect::ForEachField(value, [&](const auto& member) { ReflectedReader::WriteMember(out, member); });
    } else {
        ReflectedReader::WriteMember(out, value);
    }
}

// One entity from LUMP_ENTITIES: an ordered key/value list, Source style.
struct BSPEntity {
    std::vector<std::pair<std::string, std::string>> keys;

    [[nodiscard]] auto Find(std::string_view key) const -> std::string_view {
        for (const auto& [k, v]: keys) {
            if (k == key) {
                return v;
            }
        }
        return {};
    }

    [[nodiscard]] auto Has(std::string_view key) const -> bool { return !Find(key).empty(); }

    // "x y z" (or "p y r") keyvalues, Source convention. Returns false when the
    // key is missing or does not parse to exactly three floats.
    [[nodiscard]] auto FindVector(std::string_view key, float out[3]) const -> bool {
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
};

// The parsed map: every lump the marshaller needs, plus the entity list and
// resolved material names. Raw lump spans stay valid only while the caller's
// file bytes live; the struct vectors are owned copies.
struct BSPMap {
    int32_t version = 0;

    std::vector<BSPEntity> entities;

    std::vector<DPlane>    planes;
    std::vector<DVertex>   vertices;
    std::vector<DEdge>     edges;
    std::vector<int32_t>   surfEdges;
    std::vector<DFace>     faces;
    std::vector<DTexInfo>  texInfos;
    std::vector<DTexData>  texDatas;
    std::vector<std::string> texNames; // parallel to texDatas
    std::vector<DModel>    models;
    std::vector<DDispInfo> dispInfos;
    std::vector<DDispVert> dispVerts;
    std::vector<DNode>     nodes;
    std::vector<DLeaf>     leafs;
    std::vector<DBrush>    brushes;
    std::vector<DBrushSide> brushSides;
    std::vector<DOverlay>  overlays;
};

struct ParseResult {
    bool        ok = false;
    std::string error;
    BSPMap      map;
};

// Parses a whole .bsp image. Strict on the container (ident, version window,
// lump bounds) and on every struct read; empty lumps are legal and common.
[[nodiscard]] auto ParseBsp(std::span<const std::byte> bytes) -> ParseResult;

// LUMP_ENTITIES text -> entities. Exposed for tests and the calculator.
[[nodiscard]] auto ParseEntities(std::string_view text) -> std::vector<BSPEntity>;

} // namespace ZHLN::BSP
