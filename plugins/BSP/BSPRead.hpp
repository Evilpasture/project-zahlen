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
#include <array>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <expected>
#include <optional>
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
    ChecksumMismatch   ZHLN_ANNOTATION(ZHLN::Description<"Checksum mismatch between model files"> {}),
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

    // "x y z" (or "p y r") keyvalues, Source convention.
    [[nodiscard]] auto FindVector(std::string_view key) const noexcept -> std::optional<std::array<float, 3>>;
    [[nodiscard]] auto FindVector(std::string_view key, float out[3]) const noexcept -> bool;
    [[nodiscard]] auto FindFloat(std::string_view key) const noexcept -> std::optional<float>;
    [[nodiscard]] auto FindInt(std::string_view key) const noexcept -> std::optional<int32_t>;
    // spawnflags is a bit field and Source writes it unsigned; decimal values
    // above INT32_MAX would truncate through FindInt.
    [[nodiscard]] auto FindUInt(std::string_view key) const noexcept -> std::optional<uint32_t>;
};

// One static prop instance recovered from LUMP_GAMELUMP ('sprp').
struct StaticProp {
    std::string            modelName;
    std::array<float, 3>   origin {0.0f, 0.0f, 0.0f};
    std::array<float, 3>   angles {0.0f, 0.0f, 0.0f}; // pitch, yaw, roll (degrees)
    uint16_t               propType = 0;               // Model dictionary index
    uint16_t               firstLeaf = 0;
    uint16_t               leafCount = 0;
    uint8_t                solid = 6;                  // Collision mode (0=none, 2=bbox, 6=vphysics)
    uint8_t                flags = 0;
    int32_t                skin = 0;
    float                  fadeMinDist = 0.0f;
    float                  fadeMaxDist = 0.0f;
    std::array<float, 3>   lightingOrigin {0.0f, 0.0f, 0.0f};
    float                  forcedFadeScale = 1.0f;
    std::array<uint8_t, 4> diffuseModulation {255, 255, 255, 255}; // RGBA Color32
    float                  uniformScale = 1.0f;
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
    std::vector<DGameLump>   gameLumps;
    std::vector<StaticProp>  staticProps;
    std::vector<DLeafAmbientIndex>    leafAmbientIndex;   // LDR, one per leaf (may be empty)
    std::vector<DLeafAmbientLighting> leafAmbientSamples; // LDR ambient cube samples

    // LUMP_LIGHTING (or LUMP_LIGHTING_HDR when it carries data): VRAD's baked
    // radiance, one ColorRGBExp32 per luxel per style, addressed by
    // DFace::lightofs. Empty on maps compiled without a bake.
    std::vector<ColorRGBExp32> lighting;
    // True only when `lighting` came from LUMP_LIGHTING_HDR together with the
    // matching HDR face lump. Native lightmap ingestion keeps this provenance
    // so Source's LDR fallback is never silently treated as authored HDR.
    bool lightingIsHdr = false;
};

// Source VRAD ambient cube at a point (Source units). Walks the BSP node tree to
// the containing leaf, then takes the nearest stored sample in that leaf. Sample
// positions are the leaf bounds scaled by the 0..255 xyz bytes. Values are linear
// radiance, decoded like DecodeVradLuxel (c * 2^e), so they are on the same scale
// as the lightmaps. Returns false if the leaf has no samples.
inline auto SampleLeafAmbientCube(const BSPMap& map, const std::array<float, 3>& p, std::array<std::array<float, 3>, 6>& out) -> bool {
    if (map.nodes.empty() || map.leafs.empty()) {
        return false;
    }
    int32_t node = 0;
    while (node >= 0) {
        const DNode&  n  = map.nodes[static_cast<size_t>(node)];
        const DPlane& pl = map.planes[static_cast<size_t>(n.planenum)];
        const float   d  = pl.normal[0] * p[0] + pl.normal[1] * p[1] + pl.normal[2] * p[2] - pl.dist;
        node             = n.children[d >= 0.0f ? 0 : 1];
    }
    const size_t leafIdx = static_cast<size_t>(-node - 1);
    if (leafIdx >= map.leafs.size() || leafIdx >= map.leafAmbientIndex.size()) {
        return false;
    }
    const DLeafAmbientIndex idx = map.leafAmbientIndex[leafIdx];
    if (idx.sampleCount == 0) {
        return false;
    }
    const DLeaf& leaf = map.leafs[leafIdx];

    size_t chosen = static_cast<size_t>(-1);
    float  bestD2 = 0.0f;
    for (uint16_t s = 0; s < idx.sampleCount; ++s) {
        const size_t si = static_cast<size_t>(idx.firstSample) + s;
        if (si >= map.leafAmbientSamples.size()) {
            break;
        }
        const DLeafAmbientLighting& smp = map.leafAmbientSamples[si];
        const uint8_t                xyz[3] = {smp.x, smp.y, smp.z};
        float                        d2     = 0.0f;
        for (size_t a = 0; a < 3; ++a) {
            const float sp = static_cast<float>(leaf.mins[a]) + static_cast<float>(leaf.maxs[a] - leaf.mins[a]) * (static_cast<float>(xyz[a]) / 255.0f);
            d2 += (sp - p[a]) * (sp - p[a]);
        }
        if (chosen == static_cast<size_t>(-1) || d2 < bestD2) {
            chosen = si;
            bestD2 = d2;
        }
    }
    if (chosen == static_cast<size_t>(-1)) {
        return false;
    }
    const DLeafAmbientLighting& best = map.leafAmbientSamples[chosen];
    for (size_t f = 0; f < 6; ++f) {
        const float scale = std::ldexp(1.0f, static_cast<int>(best.cube[f].exponent));
        out[f]            = {static_cast<float>(best.cube[f].r) * scale, static_cast<float>(best.cube[f].g) * scale, static_cast<float>(best.cube[f].b) * scale};
    }
    return true;
}

// Parses a whole .bsp image. Strict on the container (ident, version window,
// lump bounds) and on every struct read; empty lumps are legal and common.
[[nodiscard]] auto ParseBsp(std::span<const std::byte> bytes) -> std::expected<BSPMap, ErrorCode>;

// LUMP_ENTITIES text -> entities.
[[nodiscard]] auto ParseEntities(std::string_view text) -> std::vector<BSPEntity>;

// LUMP_GAMELUMP (Lump 35) directory and static prop ('sprp') parsing.
[[nodiscard]] auto ParseGameLump(std::span<const std::byte> file, const LumpEntry& gameLumpEntry, BSPMap& map)
    -> std::expected<void, ErrorCode>;

} // namespace ZHLN::BSP
