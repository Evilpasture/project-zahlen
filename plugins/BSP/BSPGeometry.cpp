// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// plugins/BSP/BSPGeometry.cpp
//
// Face windings from surfedges, texture axes applied forward (texinfo is
// already the projection matrix -- none of the VMF inverse-recovery problem),
// displacement grids bilinearly interpolated from their four base corners, and
// lights read from the entity lump. See BSPGeometry.hpp for the contract.

#include "BSPGeometry.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ZHLN::BSP {
namespace {

constexpr float kEpsilon = 1e-6f;

// Width/height of the engine-side lightmap atlas texture (the
// *_lightmap0.png). The atlas is NOT the raw luxel page from the BSP: its
// layout is produced by repacking every face's (size+1 x size+1) sample rect
// with a deterministic shelf packer, so the importer reproduces the exact
// same packing analytically and emits UVs that land on the atlas the renderer
// actually samples. Scheme (must match the offline atlas generator exactly):
//   * one rect per face with lightofs >= 0 and sizeU,sizeV > 0, dims size+1
//   * stable sort by height descending; ties keep ascending face index
//   * next-fit shelves left to right over the atlas width, rects packed tight
constexpr int32_t kLightmapAtlasWidth  = 2048;
constexpr int32_t kLightmapAtlasHeight = 2048;

// texinfo flags, Source bspflags.h. Only the routing-relevant bits are
// listed; the rest (SURF_WARP for water, SURF_TRANS...) do not change which
// side of the render/physics split a face lands on.
constexpr int32_t kSurfSky2D   = 0x2;
constexpr int32_t kSurfSky     = 0x4;
constexpr int32_t kSurfTrigger = 0x40;
constexpr int32_t kSurfNodraw  = 0x80;
constexpr int32_t kSurfHint    = 0x100;
constexpr int32_t kSurfSkip    = 0x200;

void UpdateBounds(JPH::Float3& boundsMin, JPH::Float3& boundsMax, bool& first, JPH::Vec3 p) {
    if (first) {
        boundsMin = JPH::Float3(p.GetX(), p.GetY(), p.GetZ());
        boundsMax = JPH::Float3(p.GetX(), p.GetY(), p.GetZ());
        first     = false;
        return;
    }
    boundsMin.x = std::min(boundsMin.x, p.GetX());
    boundsMin.y = std::min(boundsMin.y, p.GetY());
    boundsMin.z = std::min(boundsMin.z, p.GetZ());
    boundsMax.x = std::max(boundsMax.x, p.GetX());
    boundsMax.y = std::max(boundsMax.y, p.GetY());
    boundsMax.z = std::max(boundsMax.z, p.GetZ());
}

// Source angle triples are (pitch, yaw, roll) degrees, pitch positive down,
// around a Z-up world. Pitch/yaw -> unit direction in source space.
[[nodiscard]] auto AngleDirection(JPH::Vec3 angles) -> JPH::Vec3 {
    const float pitch = angles.GetX() * (3.14159265358979323846f / 180.0f);
    const float yaw   = angles.GetY() * (3.14159265358979323846f / 180.0f);
    const float cp    = std::cos(pitch);
    return JPH::Vec3(cp * std::cos(yaw), cp * std::sin(yaw), -std::sin(pitch));
}

[[nodiscard]] auto ParseColor4(std::string_view text) -> std::optional<std::array<float, 4>> {
    if (text.empty()) {
        return std::nullopt;
    }
    std::array<float, 4> out {};
    const char*          cur = text.data();
    const char*          end = text.data() + text.size();
    for (size_t k = 0; k < 4; ++k) {
        while (cur < end && (*cur == ' ' || *cur == '\t')) {
            cur++;
        }
        if (cur >= end) {
            return (k == 0) ? std::nullopt : std::optional(out);
        }
        auto [ptr, ec] = std::from_chars(cur, end, out[k]);
        if (ec != std::errc {}) {
            return (k == 0) ? std::nullopt : std::optional(out);
        }
        cur = ptr;
    }
    return out;
}

// The closed vertex loop of one face, in face winding order, source space.
[[nodiscard]] auto FaceWinding(const BSPMap& map, const DFace& face) -> std::optional<std::vector<JPH::Vec3>> {
    if (face.firstedge < 0 || face.numedges < 3) {
        return std::nullopt;
    }
    if (static_cast<size_t>(face.firstedge) + static_cast<size_t>(face.numedges) > map.surfEdges.size()) {
        return std::nullopt;
    }
    std::vector<JPH::Vec3> out;
    out.reserve(static_cast<size_t>(face.numedges));
    for (int16_t k = 0; k < face.numedges; ++k) {
        const int32_t surfEdge  = map.surfEdges[static_cast<size_t>(face.firstedge) + static_cast<size_t>(k)];
        const int64_t edgeIndex = (surfEdge >= 0) ? surfEdge : -static_cast<int64_t>(surfEdge);
        if (edgeIndex < 0 || static_cast<size_t>(edgeIndex) >= map.edges.size()) {
            return std::nullopt;
        }
        const DEdge&   edge = map.edges[static_cast<size_t>(edgeIndex)];
        const uint16_t vert = (surfEdge >= 0) ? edge.v[0] : edge.v[1];
        if (vert >= map.vertices.size()) {
            return std::nullopt;
        }
        const auto& pt = map.vertices[vert].point;
        out.emplace_back(pt[0], pt[1], pt[2]);
    }
    return out;
}

void DropDuplicateCorners(std::vector<JPH::Vec3>& winding) {
    std::vector<JPH::Vec3> unique;
    unique.reserve(winding.size());
    for (const auto& point: winding) {
        if (unique.empty() || (point - unique.back()).LengthSq() > kEpsilon * kEpsilon) {
            unique.push_back(point);
        }
    }
    if (unique.size() > 1) {
        if ((unique.front() - unique.back()).LengthSq() <= kEpsilon * kEpsilon) {
            unique.pop_back();
        }
    }
    winding = std::move(unique);
}

struct FaceProjection {
    JPH::Vec3 sDir {1.0f, 0.0f, 0.0f};
    float     sOffset = 0.0f;
    JPH::Vec3 tDir {0.0f, 1.0f, 0.0f};
    float     tOffset = 0.0f;
    float     width   = 512.0f;
    float     height  = 512.0f;

    // Lightmap luxel projection (Source conventions)
    JPH::Vec3 lightmapSDir {0.0f, 0.0f, 0.0f};
    float     lightmapSOffset = 0.0f;
    JPH::Vec3 lightmapTDir {0.0f, 0.0f, 0.0f};
    float     lightmapTOffset = 0.0f;
    int32_t   lightmapMins[2] {0, 0};
    int32_t   lightmapSize[2] {0, 0};
    bool      hasLightmap = false;
    // Repacked shelf-atlas origin for this face's sample rect in the
    // engine-side 2048x2048 lightmap atlas (see ComputeLightmapAtlas); -1/-1
    // when the face has no atlas placement.
    int32_t atlasOrigin[2] {-1, -1};
};

[[nodiscard]] auto ProjectionFor(const BSPMap& map, const DFace& face) -> std::optional<FaceProjection> {
    if (face.texinfo == kTexInfoNode || face.texinfo < 0 || static_cast<size_t>(face.texinfo) >= map.texInfos.size()) {
        return std::nullopt;
    }
    const DTexInfo& texInfo = map.texInfos[static_cast<size_t>(face.texinfo)];
    if (texInfo.texdata < 0 || static_cast<size_t>(texInfo.texdata) >= map.texDatas.size()) {
        return std::nullopt;
    }
    const DTexData& texData = map.texDatas[static_cast<size_t>(texInfo.texdata)];
    FaceProjection  out;
    out.sDir    = JPH::Vec3(texInfo.textureVecs[0][0], texInfo.textureVecs[0][1], texInfo.textureVecs[0][2]);
    out.sOffset = texInfo.textureVecs[0][3];
    out.tDir    = JPH::Vec3(texInfo.textureVecs[1][0], texInfo.textureVecs[1][1], texInfo.textureVecs[1][2]);
    out.tOffset = texInfo.textureVecs[1][3];
    out.width   = (texData.width > 0) ? static_cast<float>(texData.width) : 512.0f;
    out.height  = (texData.height > 0) ? static_cast<float>(texData.height) : 512.0f;

    out.lightmapSDir    = JPH::Vec3(texInfo.lightmapVecs[0][0], texInfo.lightmapVecs[0][1], texInfo.lightmapVecs[0][2]);
    out.lightmapSOffset = texInfo.lightmapVecs[0][3];
    out.lightmapTDir    = JPH::Vec3(texInfo.lightmapVecs[1][0], texInfo.lightmapVecs[1][1], texInfo.lightmapVecs[1][2]);
    out.lightmapTOffset = texInfo.lightmapVecs[1][3];
    out.lightmapMins[0] = face.lightmapTextureMinsInLuxels[0];
    out.lightmapMins[1] = face.lightmapTextureMinsInLuxels[1];
    out.lightmapSize[0] = face.lightmapTextureSizeInLuxels[0];
    out.lightmapSize[1] = face.lightmapTextureSizeInLuxels[1];
    out.hasLightmap     = (face.lightmapTextureSizeInLuxels[0] > 0 && face.lightmapTextureSizeInLuxels[1] > 0);
    return out;
}

[[nodiscard]] auto MaterialNameFor(const BSPMap& map, const DFace& face) -> std::string_view {
    if (face.texinfo < 0 || static_cast<size_t>(face.texinfo) >= map.texInfos.size()) {
        return "unknown";
    }
    const int32_t texDataIndex = map.texInfos[static_cast<size_t>(face.texinfo)].texdata;
    if (texDataIndex < 0 || static_cast<size_t>(texDataIndex) >= map.texNames.size()) {
        return "unknown";
    }
    return map.texNames[static_cast<size_t>(texDataIndex)];
}

[[nodiscard]] auto StartsWithIgnoreCase(std::string_view text, std::string_view prefix) noexcept -> bool {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        const auto lhs = static_cast<unsigned char>(text[i]);
        const auto rhs = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

enum class FaceDisposition : uint8_t { Render, CollisionOnly, Drop };

// Source ships tool-texture brushes as ordinary world faces distinguished
// only by texinfo flags and the "tools/" material names -- VIS helpers and
// volumetric markers that must never draw (fog/trigger/hint/skip render as
// "FOG" or "TRIGGER"-lettered walls when treated as textures), plus invisible
// blockers that must still collide (nodraw undersides, sky brushes, invisible
// and clip walls). Route each face to its proper consumer:
//   Render        -> visible world mesh
//   CollisionOnly -> world physics hull only (invisible yet solid)
//   Drop          -> invisible AND non-solid, meaning literally nothing
[[nodiscard]] auto DispositionFor(const BSPMap& map, const DFace& face) -> FaceDisposition {
    if (face.texinfo < 0 || static_cast<size_t>(face.texinfo) >= map.texInfos.size()) {
        return FaceDisposition::Render;
    }
    const std::string_view name = MaterialNameFor(map, face);

    // Name checks come first: trigger/fog/hint/skip are invisible and
    // non-solid regardless of what (or whether) the flags record, and a name
    // cannot be overridden away -- it encodes the mapper's intent directly.
    constexpr std::string_view kToolsPrefix = "tools/";
    if (StartsWithIgnoreCase(name, kToolsPrefix)) {
        const std::string_view tool = name.substr(kToolsPrefix.size());
        if (StartsWithIgnoreCase(tool, "toolstrigger") || StartsWithIgnoreCase(tool, "toolshint") || StartsWithIgnoreCase(tool, "toolsskip") ||
            StartsWithIgnoreCase(tool, "toolsfog") || StartsWithIgnoreCase(tool, "toolsblocklight") || StartsWithIgnoreCase(tool, "toolsareaportal") ||
            StartsWithIgnoreCase(tool, "toolsoccluder") || StartsWithIgnoreCase(tool, "toolsblocklos") || StartsWithIgnoreCase(tool, "toolsblockbullets") ||
            StartsWithIgnoreCase(tool, "toolsnpcclip") || StartsWithIgnoreCase(tool, "toolsgrenadeclip")) {
            return FaceDisposition::Drop;
        }
        // toolsblack and toolsdotted are the two tool textures that Source
        // DOES draw in-game; everything else under tools/ is editor-only.
        if (!StartsWithIgnoreCase(tool, "toolsblack") && !StartsWithIgnoreCase(tool, "toolsdotted")) {
            return FaceDisposition::CollisionOnly;
        }
        return FaceDisposition::Render;
    }

    // Generic materials can still carry compile flags (mapper toggled them in
    // Hammer's face editor); honour the flag-based classes for those.
    const int32_t flags = map.texInfos[static_cast<size_t>(face.texinfo)].flags;
    if ((flags & (kSurfTrigger | kSurfHint | kSurfSkip)) != 0) {
        return FaceDisposition::Drop;
    }
    if ((flags & (kSurfSky | kSurfSky2D | kSurfNodraw)) != 0) {
        return FaceDisposition::CollisionOnly;
    }
    return FaceDisposition::Render;
}

auto FindOrMakePart(ImportedMapData& result, const std::string& name) -> MaterialStreams& {
    for (MaterialStreams& part: result.parts) {
        if (part.materialName == name) {
            return part;
        }
    }
    result.parts.push_back(MaterialStreams {});
    result.parts.back().materialName = name;
    return result.parts.back();
}

// One render-class face after emission, reduced to what the twin audit needs:
// canonical plane identity, 2D extents inside that plane, its authored front
// and the part it landed in.
//
// Source compiles a closed BSP out of the brush set, but mappers routinely
// build single-sheet walls whose ONLY face points away from the room it
// bounds (the room-facing half is left out entirely -- common on 2010s-era
// ported maps built as poster boxes). vbsp still lists such a face in the
// leaffaces of the leaves on its back side, and Source's world pass draws it
// from there; a strictly front-only renderer instead culls it, so the room
// sees straight through. Rigid bodies fall through the same faces from the
// back while EBackFaceMode::CollideWithBackFaces hides the hole from the
// CharacterVirtual -- the classic symptom is "wall invisible from inside,
// props pushed at it vanish into the void" (confirmed empirically on
// gm_murder_drones: every such face clusters inside the spawn building, and
// the spawn-room leaves reference them from their back side).
//
// The audit pairs kept render faces on the same plane; a face without an
// opposite-facing twin covering it gets the double-sided treatment: its whole
// material group renders both ways, and reversed copies of its triangles join
// the collision stream so dynamic bodies contact the back side too.
struct TwinRecord {
    double                 cn[3] {0.0, 0.0, 0.0};                      // canonical plane normal (dominant component positive)
    double                 cd = 0.0;                                   // plane constant in the canonical frame
    double                 ownN[3] {0.0, 0.0, 0.0};                    // authored front (Source coords)
    double                 uLo = 0.0, uHi = 0.0, vLo = 0.0, vHi = 0.0; // extents in canonical plane axes
    double                 area      = 0.0;
    size_t                 partIndex = 0;
    std::vector<JPH::Vec3> winding; // Source-space corners in emitted (post-flip) order
};

// Twin matching tolerance grouping: normals quantized to 0.01, plane constant
// to 4u so genuinely coplanar sheets (vbsp emits them bitwise-identical) land
// in one bucket without pulling parallel-but-different walls in.
[[nodiscard]] auto CanonicalTwinKey(const double cn[3], double cd) -> uint64_t {
    auto     quant = [](double v, double step) -> int64_t { return static_cast<int64_t>(std::llround(v / step)); };
    uint64_t h     = 1469598103934665603ull;
    auto     mix   = [&h](uint64_t x) {
        h ^= x;
        h *= 1099511628211ull;
    };
    mix(static_cast<uint64_t>(quant(cn[0], 0.01)) * 1315423911ull);
    mix(static_cast<uint64_t>(quant(cn[1], 0.01)) * 2654435761ull);
    mix(static_cast<uint64_t>(quant(cn[2], 0.01)) * 97531ull);
    mix(static_cast<uint64_t>(quant(cd, 4.0)) * 31ull);
    return h;
}

// CONTENTS_SOLID per Source; used to detect open space behind a face.
constexpr int32_t kContentsSolid = 0x1;

// BSPTree point classification against model 0's head node: walks split
// planes down to a leaf and reports its contents. Mirrors Source's own "what
// volume is this point in" query; runs in Source coordinates.
[[nodiscard]] auto ContentsAtPoint(const BSPMap& map, const JPH::Vec3& p) noexcept -> int32_t {
    if (map.models.empty() || map.nodes.empty()) {
        return kContentsSolid;
    }
    int32_t node = map.models.front().headnode;
    for (int guard = 0; guard < 8192; ++guard) {
        if (node < 0) {
            const size_t leaf = static_cast<size_t>(-1 - node);
            return leaf < map.leafs.size() ? map.leafs[leaf].contents : kContentsSolid;
        }
        if (static_cast<size_t>(node) >= map.nodes.size()) {
            return kContentsSolid;
        }
        const DNode& n = map.nodes[static_cast<size_t>(node)];
        if (n.planenum < 0 || static_cast<size_t>(n.planenum) >= map.planes.size()) {
            return kContentsSolid;
        }
        const DPlane& plane = map.planes[static_cast<size_t>(n.planenum)];
        const float   dist  = p.GetX() * plane.normal[0] + p.GetY() * plane.normal[1] + p.GetZ() * plane.normal[2] - plane.dist;
        node                = n.children[dist >= 0.0f ? 0 : 1];
    }
    return kContentsSolid;
}

// Marks every undercovered face's part double-sided and appends reversed
// triangles to the collision stream (engine coords, matching the rest of
// result.collisionOnly). A face only qualifies when open air exists BEHIND
// it: without that gate every exterior wall backside and every sealed void
// skin matches "no twin" and we'd force most of the map double-sided. With
// it, the marks land exactly on single-sheet walls players can stand behind
// (gm_murder_drones: ~5000 audit hits, only the spawn-building subset is
// reachable, the rest sits in sealed chases -- harmless but inclusive).
void ApplyTwinAudit(const BSPMap& map, ImportedMapData& result, std::vector<TwinRecord>& records, const ImportOptions& options) {
    if (records.empty()) {
        return;
    }
    constexpr double                                  kMinSheetArea   = 128.0;
    constexpr double                                  kCoverThreshold = 0.25;
    std::unordered_map<uint64_t, std::vector<size_t>> buckets;
    buckets.reserve(records.size());
    for (size_t i = 0; i < records.size(); ++i) {
        buckets[CanonicalTwinKey(records[i].cn, records[i].cd)].push_back(i);
    }
    for (auto& kv: buckets) {
        for (const size_t a: kv.second) {
            TwinRecord& rec = records[a];
            if (rec.area < kMinSheetArea) {
                continue;
            }
            double cover = 0.0;
            for (const size_t b: kv.second) {
                if (b == a) {
                    continue;
                }
                const TwinRecord& other = records[b];
                const double      dot   = rec.ownN[0] * other.ownN[0] + rec.ownN[1] * other.ownN[1] + rec.ownN[2] * other.ownN[2];
                if (dot > -0.5) {
                    continue; // twin must FACE the opposite way
                }
                const double ovU = std::min(rec.uHi, other.uHi) - std::max(rec.uLo, other.uLo);
                const double ovV = std::min(rec.vHi, other.vHi) - std::max(rec.vLo, other.vLo);
                if (ovU > 0.0 && ovV > 0.0) {
                    cover += ovU * ovV;
                }
            }
            const double extent = (rec.uHi - rec.uLo) * (rec.vHi - rec.vLo);
            if (extent <= 0.0 || cover / extent >= kCoverThreshold) {
                continue;
            }
            // Open air behind the face (centroid and a few edge midpoints,
            // sampled close and far)? Without it nobody can ever see or touch
            // the back side, so the twin-less sheets is fine single-sided.
            const JPH::Vec3 n(rec.ownN[0], rec.ownN[1], rec.ownN[2]);
            JPH::Vec3       centre = JPH::Vec3::sZero();
            for (const JPH::Vec3& corner: rec.winding) {
                centre += corner;
            }
            centre          = centre / static_cast<float>(rec.winding.size());
            bool backIsOpen = false;
            auto probeBack  = [&](const JPH::Vec3& at, float distance) {
                const int32_t contents = ContentsAtPoint(map, at - n * distance);
                if ((contents & kContentsSolid) == 0) {
                    backIsOpen = true;
                }
            };
            probeBack(centre, 2.0f);
            if (!backIsOpen) {
                probeBack(centre, 8.0f);
            }
            for (size_t i = 1; i < rec.winding.size() && !backIsOpen; i += 2) {
                const JPH::Vec3 mid = (rec.winding[i - 1] + rec.winding[i]) * 0.5f;
                probeBack(mid, 2.0f);
            }
            if (!backIsOpen) {
                continue;
            }
            result.parts[rec.partIndex].forceDoubleSided = true;
            ++result.stats.singleSidedSheets;
            if (!options.buildColliders) {
                continue;
            }
            MaterialStreams& coll = result.collisionOnly;
            const uint32_t   base = coll.VertexCount();
            for (const JPH::Vec3& corner: rec.winding) {
                const JPH::Vec3 p = ConvertPosition(options, corner);
                coll.positions.push_back(VertexPosition {{p.GetX(), p.GetY(), p.GetZ()}});
            }
            for (size_t i = 1; i + 1 < rec.winding.size(); ++i) {
                coll.indices.push_back(base);
                coll.indices.push_back(base + static_cast<uint32_t>(i) + 1u);
                coll.indices.push_back(base + static_cast<uint32_t>(i));
            }
        }
    }
}

// Emits one vertex: position/normal/tangent converted to engine space, UVs from
// the forward texture projection in source space. Bounds are tracked on both
// the owning part and the whole map as positions are emitted.
void EmitVertex(
    MaterialStreams&      part,
    const ImportOptions&  options,
    const FaceProjection& projection,
    JPH::Vec3             sourcePos,
    JPH::Vec3             sourceNormal,
    ImportedMapData&      result,
    bool&                 partBoundsFirst,
    bool&                 mapBoundsFirst
) {
    const float u = (sourcePos.Dot(projection.sDir) + projection.sOffset) / projection.width;
    const float v = (sourcePos.Dot(projection.tDir) + projection.tOffset) / projection.height;

    float lmU = 0.0f;
    float lmV = 0.0f;
    if (projection.hasLightmap) {
        // dot(p, lightmapS/TDir) + offset IS the absolute luxel coordinate in
        // the shared lightmap page (a point on the face's mins corner maps to
        // exactly lightmapTextureMins), so dividing by the page extent gives
        // atlas UVs in the same packing the baked *_lightmapN.png images and
        // every third-party lightmap dumper use. Normalizing per-face (the
        // old code, mins/size) instead stretches the whole atlas across each
        // quad -- faces then sample random atlas texels and wrong surfaces
        // render pitch black or glowing, looking "missing".
        const float sLuxel = sourcePos.Dot(projection.lightmapSDir) + projection.lightmapSOffset;
        const float tLuxel = sourcePos.Dot(projection.lightmapTDir) + projection.lightmapTOffset;
        if (projection.atlasOrigin[0] >= 0) {
            // dot(p, lightmapS/TDir) + offset is the absolute luxel coordinate
            // in the raw page; subtracting the face's mins corner gives the
            // in-rect luxel, and the repacked atlas origin places it on the
            // 2048x2048 engine atlas alongside the baked image.
            lmU = std::clamp((projection.atlasOrigin[0] + (sLuxel - projection.lightmapMins[0]) + 0.5f) / kLightmapAtlasWidth, 0.0f, 1.0f);
            lmV = std::clamp((projection.atlasOrigin[1] + (tLuxel - projection.lightmapMins[1]) + 0.5f) / kLightmapAtlasHeight, 0.0f, 1.0f);
        } else {
            lmU = (result.lightmapPageWidth > 0.0f) ? std::clamp((sLuxel + 0.5f) / result.lightmapPageWidth, 0.0f, 1.0f) : 0.0f;
            lmV = (result.lightmapPageHeight > 0.0f) ? std::clamp((tLuxel + 0.5f) / result.lightmapPageHeight, 0.0f, 1.0f) : 0.0f;
        }
    }

    const JPH::Vec3 tangent = (projection.sDir.LengthSq() > kEpsilon * kEpsilon) ? projection.sDir.Normalized() : JPH::Vec3::sAxisX();
    const JPH::Vec3 normal  = (sourceNormal.LengthSq() > kEpsilon * kEpsilon) ? sourceNormal.Normalized() : JPH::Vec3::sAxisZ();

    // Handedness so that bitangent = cross(normal, tangent.xyz) * tangent.w,
    // which is the glTF convention the renderer's tangent frames assume.
    const JPH::Vec3 crossNT    = normal.Cross(tangent);
    const float     handedness = (crossNT.Dot(projection.tDir) < 0.0f) ? -1.0f : 1.0f;

    const JPH::Vec3 enginePos     = ConvertPosition(options, sourcePos);
    const JPH::Vec3 engineNormal  = ConvertDirection(options, normal);
    const JPH::Vec3 engineTangent = ConvertDirection(options, tangent);

    part.positions.push_back(VertexPosition {{enginePos.GetX(), enginePos.GetY(), enginePos.GetZ()}});
    part.tangentFrames.push_back(
        VertexTangentFrame {
            Math::PackNormal(engineNormal.GetX(), engineNormal.GetY(), engineNormal.GetZ()),
            Math::PackNormal(engineTangent.GetX(), engineTangent.GetY(), engineTangent.GetZ(), handedness),
        }
    );
    part.surfaces.push_back(VertexSurface {Math::PackUV(u, v), Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f), Math::PackUV(lmU, lmV)});

    UpdateBounds(part.boundsMin, part.boundsMax, partBoundsFirst, enginePos);
    UpdateBounds(result.boundsMin, result.boundsMax, mapBoundsFirst, enginePos);
}

[[nodiscard]] auto GatherLights(const BSPMap& map, const ImportOptions& options) -> std::vector<BspPointLight> {
    std::vector<BspPointLight> out;
    for (const BSPEntity& entity: map.entities) {
        const std::string_view className = entity.Find("classname");
        LightType              type;
        if (className == "light") {
            type = LightType::Point;
        } else if (className == "light_spot") {
            type = LightType::Spot;
        } else if (className == "light_environment") {
            type = LightType::Directional;
        } else {
            continue;
        }

        BspPointLight light;
        light.type = type;
        light.name = std::string(entity.Find("targetname"));
        if (light.name.empty()) {
            light.name = std::string(className);
        }

        const auto originArr = entity.FindVector("origin").value_or(std::array<float, 3> {0.0f, 0.0f, 0.0f});
        light.position       = ConvertPosition(options, JPH::Vec3(originArr[0], originArr[1], originArr[2]));

        const auto color4 = ParseColor4(entity.Find("_light")).value_or(std::array<float, 4> {255.0f, 255.0f, 255.0f, 200.0f});
        light.color =
            JPH::Vec3(std::clamp(color4[0] / 255.0f, 0.0f, 1.0f), std::clamp(color4[1] / 255.0f, 0.0f, 1.0f), std::clamp(color4[2] / 255.0f, 0.0f, 1.0f));
        light.intensity = color4[3] * kSourceBrightnessToEngine;

        auto anglesArr = entity.FindVector("angles").value_or(std::array<float, 3> {0.0f, 0.0f, 0.0f});
        if (entity.Find("angles").empty()) {
            if (const auto pitch = entity.FindFloat("pitch")) {
                anglesArr[0] = *pitch;
            }
        }
        const JPH::Vec3 direction = AngleDirection(JPH::Vec3(anglesArr[0], anglesArr[1], anglesArr[2]));
        light.direction           = ConvertDirection(options, direction);

        if (type == LightType::Spot) {
            if (const auto outerDegrees = entity.FindFloat("_cone"); outerDegrees && *outerDegrees > 0.0f) {
                light.outerConeRadians = *outerDegrees * (3.14159265358979323846f / 180.0f);
            }
            if (const auto innerDegrees = entity.FindFloat("_cone2"); innerDegrees && *innerDegrees > 0.0f) {
                light.innerConeRadians = *innerDegrees * (3.14159265358979323846f / 180.0f);
            }
        }

        out.push_back(std::move(light));
    }
    return out;
}

} // namespace

auto ConvertPosition(const ImportOptions& options, JPH::Vec3 in) noexcept -> JPH::Vec3 {
    const float s = options.unitScale;
    if (!options.convertCoordinates) {
        return in * s;
    }
    // Z-up right-handed -> Y-up right-handed: rotate -90 degrees about X,
    // then scale. (x, y, z) -> (x, z, -y).
    return JPH::Vec3(in.GetX() * s, in.GetZ() * s, -in.GetY() * s);
}

auto ConvertDirection(const ImportOptions& options, JPH::Vec3 in) noexcept -> JPH::Vec3 {
    if (!options.convertCoordinates) {
        return in;
    }
    return JPH::Vec3(in.GetX(), in.GetZ(), -in.GetY());
}

struct LightmapAtlasEntry {
    int32_t x = -1;
    int32_t y = -1;
};

[[nodiscard]] auto ComputeLightmapAtlas(const BSPMap& map) -> std::vector<LightmapAtlasEntry> {
    struct Rect {
        int32_t w, h, faceIndex;
    };
    std::vector<Rect> rects;
    rects.reserve(map.faces.size());
    for (size_t i = 0; i < map.faces.size(); ++i) {
        const DFace& face = map.faces[i];
        if (face.lightofs < 0 || face.lightmapTextureSizeInLuxels[0] <= 0 || face.lightmapTextureSizeInLuxels[1] <= 0) {
            continue;
        }
        rects.push_back({face.lightmapTextureSizeInLuxels[0] + 1, face.lightmapTextureSizeInLuxels[1] + 1, static_cast<int32_t>(i)});
    }
    std::stable_sort(rects.begin(), rects.end(), [](const Rect& a, const Rect& b) { return a.h > b.h; });

    std::vector<LightmapAtlasEntry> placements(map.faces.size());
    int32_t                         x = 0, y = 0, shelfH = 0;
    for (const Rect& r: rects) {
        if (x + r.w > kLightmapAtlasWidth) {
            x = 0;
            y += shelfH;
            shelfH = 0;
        }
        placements[static_cast<size_t>(r.faceIndex)] = {x, y};
        x += r.w;
        shelfH = std::max(shelfH, r.h);
    }
    return placements;
}

auto CollectInteractiveSubmodels(const BSPMap& map, const ImportOptions& options) -> std::vector<InteractiveSubmodel> {
    std::vector<InteractiveSubmodel> result;

    for (const BSPEntity& entity: map.entities) {
        const std::string_view className = entity.Find("classname");
        const bool             isDoor =
            (className == "func_door" || className == "func_door_rotating" || className == "prop_door_rotating" || className == "momentary_door");
        const bool isButton = (className == "func_button" || className == "func_rot_button" || className == "momentary_rot_button");
        if (!isDoor && !isButton) {
            continue;
        }

        const std::string_view modelName = entity.Find("model");
        if (modelName.size() < 2 || modelName[0] != '*') {
            continue;
        }
        int32_t modelIndex = -1;
        std::from_chars(modelName.data() + 1, modelName.data() + modelName.size(), modelIndex);
        // Model 0 is worldspawn; brush entities reference submodels 1..N.
        if (modelIndex < 1 || static_cast<size_t>(modelIndex) >= map.models.size()) {
            continue;
        }
        if (std::any_of(result.begin(), result.end(), [&](const InteractiveSubmodel& sub) { return sub.modelIndex == modelIndex; })) {
            continue;
        }

        InteractiveSubmodel sub;
        sub.modelIndex = modelIndex;
        sub.rotating   = (className == "func_door_rotating" || className == "func_rot_button" || className == "momentary_rot_button");

        const DModel&   bspModel = map.models[static_cast<size_t>(modelIndex)];
        const JPH::Vec3 mins(bspModel.mins[0], bspModel.mins[1], bspModel.mins[2]);
        const JPH::Vec3 maxs(bspModel.maxs[0], bspModel.maxs[1], bspModel.maxs[2]);
        JPH::Vec3       pivot = (mins + maxs) * 0.5f;
        if (const auto originKey = entity.FindVector("origin")) {
            pivot = JPH::Vec3((*originKey)[0], (*originKey)[1], (*originKey)[2]);
        }
        sub.pivot = ConvertPosition(options, pivot);

        result.push_back(sub);
    }

    return result;
}

auto ImportMapGeometry(const BSPMap& map, const ImportOptions& options) -> ImportedMapData {
    ImportedMapData result;
    bool            globalBoundsFirst = true;

    // Post-pass input for the single-sheet wall audit (one record per
    // undisplaced render face; see ApplyTwinAudit).
    std::vector<TwinRecord> twinRecords;

    // Lightmap atlas: reproduced analytically from the same shelf packer the
    // offline atlas generator used to lay out the *_lightmap0.png (see
    // ComputeLightmapAtlas). Every vertex's lightmap UV is then expressed in
    // that atlas' coordinate space. The raw-BSP mins/page size is NOT the
    // atlas layout: raw-page UVs (the old code) scatter each face's samples
    // over unrelated rectangles -- surfaces read black or glow at random.
    const std::vector<LightmapAtlasEntry> lightmapAtlas = ComputeLightmapAtlas(map);

    // Mirror the shelf pack back out for the importer: it decodes VRAD luxels
    // into an atlas texture whose texels must land on exactly these cells.
    result.lightmapOrigins.reserve(lightmapAtlas.size());
    for (const LightmapAtlasEntry& entry: lightmapAtlas) {
        result.lightmapOrigins.push_back({entry.x, entry.y});
    }
    for (const DFace& face: map.faces) {
        if (face.lightofs >= 0 && face.lightmapTextureSizeInLuxels[0] > 0 && face.lightmapTextureSizeInLuxels[1] > 0) {
            result.lightmapPageWidth  = kLightmapAtlasWidth;
            result.lightmapPageHeight = kLightmapAtlasHeight;
            break;
        }
    }

    for (size_t modelIdx = 0; modelIdx < map.models.size(); ++modelIdx) {
        if (options.onlyModel >= 0 && static_cast<int32_t>(modelIdx) != options.onlyModel) {
            continue;
        }
        if (std::find(options.skipModels.begin(), options.skipModels.end(), static_cast<int32_t>(modelIdx)) != options.skipModels.end()) {
            continue;
        }
        const DModel& model = map.models[modelIdx];
        if (model.firstface < 0 || model.numfaces < 0) {
            continue;
        }
        const size_t faceEnd = std::min(static_cast<size_t>(model.firstface) + static_cast<size_t>(model.numfaces), map.faces.size());
        for (size_t faceIndex = static_cast<size_t>(model.firstface); faceIndex < faceEnd; ++faceIndex) {
            const DFace& face = map.faces[faceIndex];

            auto winding = FaceWinding(map, face);
            if (!winding) {
                ++result.stats.droppedNoWinding;
                continue;
            }
            DropDuplicateCorners(*winding);
            if (winding->size() < 3) {
                ++result.stats.droppedDegenerate;
                continue;
            }

            // Orient the winding so triangle fronts face the face's own front,
            // which is the PLANE normal: vbsp stores every brush face's plane
            // with +N pointing out of the solid while the stored surfedge loop
            // runs the other way (CW about +N) for BOTH side values, so the
            // geometric fix-up below reverses it. The `side` bit must NOT enter
            // the rule -- it is tree bookkeeping, not the authored front:
            // auditing gm_murder_drones against the BSP's own leaf contents
            // (open air vs CONTENTS_SOLID a few units off each face) puts the
            // air side on +N for 20414 of 21024 decidable faces across both
            // side values; flipping on side==1 inverts ~8.6k of them (room-side
            // invisible under backface culling, normals lit from inside the
            // wall, and twin-audit back-probes aimed at the wrong side). Both
            // consumers key off the result: the renderer culls back faces (a
            // wrong-side interior wall is culled away and invisible from
            // inside), and the world Jolt mesh collider made from these same
            // triangles only contacts rigid bodies on triangle fronts (an
            // inverted floor drops props straight through; the
            // CharacterVirtual walks anywhere only because it opts into
            // EBackFaceMode::CollideWithBackFaces).
            JPH::Vec3 faceNormal = JPH::Vec3::sAxisZ();
            if (static_cast<size_t>(face.planenum) < map.planes.size()) {
                const DPlane& plane = map.planes[static_cast<size_t>(face.planenum)];
                faceNormal          = JPH::Vec3(plane.normal[0], plane.normal[1], plane.normal[2]);
            }
            {
                JPH::Vec3 windingNormal = JPH::Vec3::sZero();
                for (size_t i = 0; i < winding->size(); ++i) {
                    const JPH::Vec3& a = (*winding)[i];
                    const JPH::Vec3& b = (*winding)[(i + 1) % winding->size()];
                    windingNormal += JPH::Vec3(
                        (a.GetY() - b.GetY()) * (a.GetZ() + b.GetZ()), (a.GetZ() - b.GetZ()) * (a.GetX() + b.GetX()),
                        (a.GetX() - b.GetX()) * (a.GetY() + b.GetY())
                    );
                }
                if (windingNormal.Dot(faceNormal) < 0.0f) {
                    std::reverse(winding->begin(), winding->end());
                }
            }

            // Tool-texture classes: skip fog/trigger/hint entirely (they are
            // editor markers, not surfaces), and route invisible-but-solid
            // faces (nodraw, sky, clip) to the collision stream so the physics
            // hull matches Source even though no pixels are produced. When no
            // collider is being built the collision stream is unused --
            // dropping the faces there saves the vertex emission work too.
            const FaceDisposition disposition = DispositionFor(map, face);
            if (disposition == FaceDisposition::Drop) {
                ++result.stats.helperFacesDropped;
                continue;
            }
            if (disposition == FaceDisposition::CollisionOnly && !options.buildColliders) {
                ++result.stats.helperFacesDropped;
                continue;
            }

            auto projection = ProjectionFor(map, face);
            if (!projection) {
                ++result.stats.droppedNoProjection;
                continue;
            }
            if (projection->hasLightmap && faceIndex < lightmapAtlas.size() && lightmapAtlas[faceIndex].x >= 0) {
                projection->atlasOrigin[0] = lightmapAtlas[faceIndex].x;
                projection->atlasOrigin[1] = lightmapAtlas[faceIndex].y;
            }

            const std::string_view materialName = MaterialNameFor(map, face);
            MaterialStreams& part = (disposition == FaceDisposition::CollisionOnly) ? result.collisionOnly : FindOrMakePart(result, std::string(materialName));
            if (disposition == FaceDisposition::CollisionOnly && part.VertexCount() == 0) {
                part.materialName = std::string(materialName); // informational only; the importer does not key off it
            }

            const uint32_t baseVertex      = part.VertexCount();
            bool           partBoundsFirst = (baseVertex == 0);

            // A face with a dispinfo record whose contents are bogus (power
            // outside 1-4, or a dispVert range that runs past the lump) must
            // not sink the whole face: the base winding still exists, so it
            // falls through to the flat-quad path with a stat bump. Compiled
            // maps do ship these, and every community converter renders the
            // base quad.
            bool displaced = options.includeDisplacements && face.dispinfo >= 0 && static_cast<size_t>(face.dispinfo) < map.dispInfos.size() &&
                             winding->size() == 4;
            if (displaced) {
                const DDispInfo& dispInfo  = map.dispInfos[static_cast<size_t>(face.dispinfo)];
                const int32_t    power     = dispInfo.power;
                bool             rangeFits = false;
                if (power >= 1 && power <= 4) {
                    const size_t side = static_cast<size_t>((1u << static_cast<uint32_t>(power)) + 1u);
                    rangeFits         = static_cast<size_t>(dispInfo.dispVertStart) + side * side <= map.dispVerts.size();
                }
                if (!rangeFits) {
                    displaced = false;
                    ++result.stats.displacementFallbacks;
                }
            }

            if (displaced) {
                const DDispInfo& dispInfo = map.dispInfos[static_cast<size_t>(face.dispinfo)];
                const int32_t    power    = dispInfo.power;
                if (power < 1 || power > 4) {
                    continue;
                }
                const uint32_t gridSize = 1u << static_cast<uint32_t>(power);
                const uint32_t side     = gridSize + 1;

                const JPH::Vec3 dispStartPos(dispInfo.startPosition[0], dispInfo.startPosition[1], dispInfo.startPosition[2]);
                // Rotate the base quad so corner 0 is the displacement's start
                // position, keeping winding order.
                size_t startIndex = 0;
                float  bestDistSq = 1e30f;
                for (size_t c = 0; c < 4; ++c) {
                    const float distSq = ((*winding)[c] - dispStartPos).LengthSq();
                    if (distSq < bestDistSq) {
                        bestDistSq = distSq;
                        startIndex = c;
                    }
                }
                const std::array<JPH::Vec3, 4> corners = {
                    (*winding)[startIndex],
                    (*winding)[(startIndex + 1) % 4],
                    (*winding)[(startIndex + 2) % 4],
                    (*winding)[(startIndex + 3) % 4],
                };

                const size_t vertBase = static_cast<size_t>(dispInfo.dispVertStart);
                if (vertBase + static_cast<size_t>(side) * side > map.dispVerts.size()) {
                    continue;
                }

                for (uint32_t j = 0; j < side; ++j) {
                    const float fv = static_cast<float>(j) / static_cast<float>(gridSize);
                    for (uint32_t i = 0; i < side; ++i) {
                        const float fu = static_cast<float>(i) / static_cast<float>(gridSize);

                        const JPH::Vec3 row0 = corners[0] + (corners[1] - corners[0]) * fu;
                        const JPH::Vec3 row1 = corners[3] + (corners[2] - corners[3]) * fu;
                        const JPH::Vec3 base = row0 + (row1 - row0) * fv;

                        const DDispVert& dispVert = map.dispVerts[vertBase + static_cast<size_t>(j) * side + i];
                        const JPH::Vec3  dispVec(dispVert.vec[0], dispVert.vec[1], dispVert.vec[2]);
                        const JPH::Vec3  sourcePos = base + dispVec * dispVert.dist;

                        EmitVertex(part, options, *projection, sourcePos, faceNormal, result, partBoundsFirst, globalBoundsFirst);
                    }
                }

                for (uint32_t j = 0; j < gridSize; ++j) {
                    for (uint32_t i = 0; i < gridSize; ++i) {
                        const uint32_t a = baseVertex + j * side + i;
                        const uint32_t b = a + 1;
                        const uint32_t c = a + side + 1;
                        const uint32_t d = a + side;
                        part.indices.push_back(a);
                        part.indices.push_back(b);
                        part.indices.push_back(c);
                        part.indices.push_back(a);
                        part.indices.push_back(c);
                        part.indices.push_back(d);
                    }
                }
                if (disposition != FaceDisposition::CollisionOnly) {
                    ++result.displacementCount;
                    result.triangleCount += gridSize * gridSize * 2;
                }
            } else {
                // Fan triangulation: Source faces are convex planar polygons.
                for (const auto& corner: *winding) {
                    EmitVertex(part, options, *projection, corner, faceNormal, result, partBoundsFirst, globalBoundsFirst);
                }
                for (size_t i = 1; i + 1 < winding->size(); ++i) {
                    part.indices.push_back(baseVertex);
                    part.indices.push_back(baseVertex + static_cast<uint32_t>(i));
                    part.indices.push_back(baseVertex + static_cast<uint32_t>(i) + 1u);
                }
                // The collision stream is invisible -- keep the public render
                // counters to faces the player can actually see.
                if (disposition != FaceDisposition::CollisionOnly) {
                    result.triangleCount += static_cast<uint32_t>(winding->size()) - 2;
                }
            }

            // Remember every undisplaced render face for the post-pass that
            // detects single-sheet walls (see TwinRecord).
            if (disposition == FaceDisposition::Render && !displaced) {
                TwinRecord rec;
                rec.partIndex   = static_cast<size_t>(&part - result.parts.data());
                rec.ownN[0]     = faceNormal.GetX();
                rec.ownN[1]     = faceNormal.GetY();
                rec.ownN[2]     = faceNormal.GetZ();
                const double nx = rec.ownN[0], ny = rec.ownN[1], nz = rec.ownN[2];
                const double ax = std::abs(nx), ay = std::abs(ny), az = std::abs(nz);
                const int    dom        = (ax >= ay && ax >= az) ? 0 : (ay >= az ? 1 : 2);
                const double domV       = dom == 0 ? nx : (dom == 1 ? ny : nz);
                const double sgn        = domV < 0.0 ? -1.0 : 1.0;
                rec.cn[0]               = nx * sgn;
                rec.cn[1]               = ny * sgn;
                rec.cn[2]               = nz * sgn;
                rec.winding             = *winding;
                const JPH::Vec3& anchor = rec.winding.front();
                rec.cd                  = rec.cn[0] * anchor.GetX() + rec.cn[1] * anchor.GetY() + rec.cn[2] * anchor.GetZ();
                const int au = (dom + 1) % 3, av = (dom + 2) % 3;
                auto      comp = [](const JPH::Vec3& p, int axis) { return axis == 0 ? p.GetX() : (axis == 1 ? p.GetY() : p.GetZ()); };
                rec.uLo = rec.vLo = 1e30;
                rec.uHi = rec.vHi = -1e30;
                for (const JPH::Vec3& p: rec.winding) {
                    const float uu = comp(p, au), vv = comp(p, av);
                    rec.uLo = std::min(rec.uLo, static_cast<double>(uu));
                    rec.uHi = std::max(rec.uHi, static_cast<double>(uu));
                    rec.vLo = std::min(rec.vLo, static_cast<double>(vv));
                    rec.vHi = std::max(rec.vHi, static_cast<double>(vv));
                }
                for (size_t i = 1; i + 1 < rec.winding.size(); ++i) {
                    const JPH::Vec3 a  = rec.winding[i] - rec.winding[0];
                    const JPH::Vec3 b  = rec.winding[i + 1] - rec.winding[0];
                    const JPH::Vec3 cr = a.Cross(b);
                    rec.area += 0.5 * static_cast<double>(cr.Length());
                }
                twinRecords.push_back(std::move(rec));
            }

            if (disposition != FaceDisposition::CollisionOnly) {
                ++result.faceCount;
            } else {
                ++result.stats.collisionOnlyFaces;
            }
        }
    }

    // Single-sheet wall audit: undercovered faces get double-sided render and
    // reversed backside triangles in the physics hull.
    ApplyTwinAudit(map, result, twinRecords, options);

    if (options.gatherLights) {
        result.lights = GatherLights(map, options);
    }
    return result;
}

} // namespace ZHLN::BSP
