// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// plugins/BSP/BSPGeometry.cpp
//
// Face windings from surfedges, texture axes applied forward (texinfo is
// already the projection matrix -- none of the VMF inverse-recovery problem),
// displacement grids bilinearly interpolated from their four base corners,
// lights read from the entity lump, and self-illuminated surfaces accumulated
// into area-light proxies. See BSPGeometry.hpp for the contract and
// BSPLighting.hpp for the lighting rules.

#include "BSPGeometry.hpp"

#include "BSPBakeField.hpp"
#include "BSPLighting.hpp"

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

[[nodiscard]] auto DropDuplicateCorners(std::span<const JPH::Vec3> winding) -> std::vector<JPH::Vec3> {
    std::vector<JPH::Vec3> unique;
    unique.reserve(winding.size());
    for (const JPH::Vec3& point: winding) {
        if (unique.empty() || (point - unique.back()).LengthSq() > kEpsilon * kEpsilon) {
            unique.push_back(point);
        }
    }
    if (unique.size() > 1 && (unique.front() - unique.back()).LengthSq() <= kEpsilon * kEpsilon) {
        unique.pop_back();
    }
    return unique;
}

struct FaceProjection {
    JPH::Vec3 sDir {1.0f, 0.0f, 0.0f};
    float     sOffset = 0.0f;
    JPH::Vec3 tDir {0.0f, 1.0f, 0.0f};
    float     tOffset = 0.0f;
    float     width   = 512.0f;
    float     height  = 512.0f;


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

// Vertex centroid of a winding (unweighted -- the caller weights by area).
[[nodiscard]] auto WindingCentroid(std::span<const JPH::Vec3> winding) noexcept -> JPH::Vec3 {
    if (winding.empty()) {
        return JPH::Vec3::sZero();
    }
    JPH::Vec3 sum = JPH::Vec3::sZero();
    for (const JPH::Vec3& corner: winding) {
        sum += corner;
    }
    return sum / static_cast<float>(winding.size());
}

// Planar area of a closed convex winding (Source faces are convex planar
// polygons), in square Source units. Used for the twin audit's coverage test
// and for the emissive surfaces' area budget.
[[nodiscard]] auto WindingArea(std::span<const JPH::Vec3> winding) noexcept -> double {
    if (winding.size() < 3) {
        return 0.0;
    }
    double area = 0.0;
    for (size_t i = 1; i + 1 < winding.size(); ++i) {
        const JPH::Vec3 a = winding[i] - winding[0];
        const JPH::Vec3 b = winding[i + 1] - winding[0];
        area += 0.5 * static_cast<double>(a.Cross(b).Length());
    }
    return area;
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

auto FindOrMakePart(ImportedMapData& result, std::string_view name, bool skyboxLayer) -> MaterialStreams& {
    for (MaterialStreams& part: result.parts) {
        if (part.materialName == name && part.skyboxLayer == skyboxLayer) {
            return part;
        }
    }
    result.parts.push_back(MaterialStreams {});
    result.parts.back().materialName = std::string(name);
    result.parts.back().skyboxLayer  = skyboxLayer;
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
void ApplyTwinAudit(const BSPMap& map, ImportedMapData& result, std::span<const TwinRecord> records, const ImportOptions& options) {
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
            const TwinRecord& rec = records[a];
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
    part.surfaces.push_back(VertexSurface {Math::PackUV(u, v), Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f), Math::PackUV(0.0f, 0.0f)});

    UpdateBounds(part.boundsMin, part.boundsMax, partBoundsFirst, enginePos);
    UpdateBounds(result.boundsMin, result.boundsMax, mapBoundsFirst, enginePos);
}

// Light harvesting and the emissive-surface rules live in BSPLighting.hpp: this
// file only supplies the geometry they read (face windings, texdata
// reflectivity, the conversion helpers above).

} // namespace

// True when the face's texinfo carries SURF_NOLIGHT (0x400). vbsp sets it on
// every surface a mapper excluded from the lightmap bake, which for a non-tool
// material means the surface is its own light source. Every self-illuminated
// face in a compiled map carries it; an ordinary wall never does.
[[nodiscard]] auto HasSurfNoLight(const BSPMap& map, const DFace& face) noexcept -> bool {
    if (face.texinfo < 0 || static_cast<size_t>(face.texinfo) >= map.texInfos.size()) {
        return false;
    }
    return (map.texInfos[static_cast<size_t>(face.texinfo)].flags & kSurfNolight) != 0;
}

// Rec.709 luma of the texdata reflectivity vbsp baked for a face's material.
// Source's `reflectivity` is the material's diffuse albedo in linear 0-1, and
// it is the only per-material brightness signal that survives into the BSP
// without a .vmt -- which is what makes it usable as an emissive gate.
[[nodiscard]] auto TexDataReflectivityLuma(const BSPMap& map, const DFace& face) noexcept -> float {
    if (face.texinfo < 0 || static_cast<size_t>(face.texinfo) >= map.texInfos.size()) {
        return 0.0f;
    }
    const int32_t texDataIndex = map.texInfos[static_cast<size_t>(face.texinfo)].texdata;
    if (texDataIndex < 0 || static_cast<size_t>(texDataIndex) >= map.texDatas.size()) {
        return 0.0f;
    }
    const auto& refl = map.texDatas[static_cast<size_t>(texDataIndex)].reflectivity;
    return refl[0] * 0.2126f + refl[1] * 0.7152f + refl[2] * 0.0722f;
}

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

namespace {

// Source AngleMatrix (SDK convention): columns are forward/left/up from
// pitch=x, yaw=y, roll=z in degrees. Stored row-major for TransformSkybox*.
void SkyboxRotation(float pitchDeg, float yawDeg, float rollDeg, float out[9]) {
    const float degToRad = 0.017453292519943295f;
    const float sp = std::sin(pitchDeg * degToRad), cp = std::cos(pitchDeg * degToRad);
    const float sy = std::sin(yawDeg * degToRad),   cy = std::cos(yawDeg * degToRad);
    const float sr = std::sin(rollDeg * degToRad),  cr = std::cos(rollDeg * degToRad);
    out[0] = cp * cy;
    out[3] = cp * sy;
    out[6] = -sp;
    const float crcy = cr * cy, crsy = cr * sy, srcy = sr * cy, srsy = sr * sy;
    out[1] = sp * srcy - crsy;
    out[4] = sp * srsy + crcy;
    out[7] = sr * cp;
    out[2] = sp * crcy + srsy;
    out[5] = sp * crsy - srcy;
    out[8] = cr * cp;
}

void ParseVec3(std::string_view text, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (text.empty()) {
        return;
    }
    const std::string copy(text);
    std::sscanf(copy.c_str(), "%f %f %f", &out[0], &out[1], &out[2]);
}

// Centroid of a face's surfedge loop, in source coordinates.
[[nodiscard]] auto FaceCentroidSource(const BSPMap& map, const DFace& face) noexcept -> std::array<float, 3> {
    std::array<float, 3> c {0.0f, 0.0f, 0.0f};
    if (face.firstedge < 0 || face.numedges <= 0) {
        return c;
    }
    const size_t end = std::min(static_cast<size_t>(face.firstedge) + static_cast<size_t>(face.numedges), map.surfEdges.size());
    size_t       n   = 0;
    for (size_t i = static_cast<size_t>(face.firstedge); i < end; ++i) {
        const int32_t se = map.surfEdges[i];
        const DEdge&  e  = map.edges[static_cast<size_t>(se < 0 ? -se : se)];
        if (e.v[0] < map.vertices.size()) {
            const float* v = map.vertices[e.v[0]].point;
            c[0] += v[0];
            c[1] += v[1];
            c[2] += v[2];
            ++n;
        }
    }
    if (n > 0) {
        c[0] /= static_cast<float>(n);
        c[1] /= static_cast<float>(n);
        c[2] /= static_cast<float>(n);
    }
    return c;
}

} // namespace

auto MakeSkyboxTransform(const BSPMap& map) noexcept -> SkyboxTransform {
    SkyboxTransform t;

    for (const BSPEntity& entity: map.entities) {
        if (entity.Find("classname") != "sky_camera") {
            continue;
        }
        ParseVec3(entity.Find("origin"), t.origin);
        float angles[3] = {0.0f, 0.0f, 0.0f};
        {
            const std::string_view angText = entity.Find("angles");
            if (!angText.empty()) {
                const std::string copy(angText);
                std::sscanf(copy.c_str(), "%f %f %f", &angles[0], &angles[1], &angles[2]);
            }
        }
        SkyboxRotation(angles[0], angles[1], angles[2], t.rot);
        const std::string_view scaleText = entity.Find("scale");
        if (!scaleText.empty()) {
            t.scale = std::strtof(std::string(scaleText).c_str(), nullptr);
            if (t.scale <= 0.0f) {
                t.scale = 16.0f;
            }
        }
        t.active = true;
        break;
    }
    if (!t.active) {
        return t;
    }

    // Anchor: the spawn the sky_camera view corresponds to. Without a player
    // start the world origin keeps the cluster's relative placement sane.
    for (const BSPEntity& entity: map.entities) {
        const std::string_view cls = entity.Find("classname");
        if (cls.starts_with("info_player")) {
            ParseVec3(entity.Find("origin"), t.anchor);
            break;
        }
    }

    // Membership floor: the skybox is a compact cluster around the sky_camera
    // separated from the map below by a z gap. Histogram face centroids near
    // the sky_camera in X/Y and walk down from its z bin to the first empty
    // bin; content is everything above that gap. Falls back to a fixed
    // 1024-unit band below the sky_camera when no gap is found.
    t.zFloor = t.origin[2] - 1024.0f;
    {
        constexpr int   kBin = 256;
        constexpr int   kBins = 48; // +-6144 units around the sky_camera z
        std::array<int, kBins> hist {};
        const int baseZ = static_cast<int>(std::floor(t.origin[2] / kBin)) - kBins / 2;
        for (const DFace& face: map.faces) {
            const auto c = FaceCentroidSource(map, face);
            if (std::abs(c[0] - t.origin[0]) > t.xyHalf || std::abs(c[1] - t.origin[1]) > t.xyHalf) {
                continue;
            }
            const int bin = static_cast<int>(std::floor(c[2] / kBin)) - baseZ;
            if (bin >= 0 && bin < kBins) {
                ++hist[static_cast<size_t>(bin)];
            }
        }
        const int skyBin = static_cast<int>(std::floor(t.origin[2] / kBin)) - baseZ;
        for (int bin = skyBin - 1; bin > 0; --bin) {
            if (hist[static_cast<size_t>(bin)] == 0) {
                t.zFloor = static_cast<float>((baseZ + bin + 1) * kBin);
                break;
            }
        }
    }

    // The world's volume: skybox content that lands inside it would
    // interpenetrate the map in a single depth buffer.
    if (!map.models.empty()) {
        for (int k = 0; k < 3; ++k) {
            t.worldMin[k] = map.models[0].mins[k];
            t.worldMax[k] = map.models[0].maxs[k];
        }
    }

    // Ground alignment: Source composes the skybox against the player's eye at
    // 1:1 scale, but the scaled miniscape's own ground sits a little below the
    // world's terrain when placed statically ("a little bit buried"). Lift the
    // whole transform so the miniscape's ground plane (the placed-z p20 of its
    // face centroids) lands at the anchor's ground level.
    {
        std::vector<float> placedZ;
        placedZ.reserve(map.faces.size() / 2);
        for (const DFace& face: map.faces) {
            const auto c = FaceCentroidSource(map, face);
            const float p[3] = {c[0], c[1], c[2]};
            if (!IsSkyboxPoint(t, p)) {
                continue;
            }
            placedZ.push_back(t.anchor[2] + (c[2] - t.origin[2]) * t.scale);
        }
        if (placedZ.size() >= 16) {
            const size_t      k = placedZ.size() / 5; // ~p20
            std::nth_element(placedZ.begin(), placedZ.begin() + static_cast<long>(k), placedZ.end());
            const float groundZ = t.anchor[2];
            t.anchor[2] += groundZ - placedZ[k];
        }
    }
    return t;
}

auto IsSkyboxPoint(const SkyboxTransform& t, const float p[3]) noexcept -> bool {
    if (!t.active) {
        return false;
    }
    return std::abs(p[0] - t.origin[0]) <= t.xyHalf && std::abs(p[1] - t.origin[1]) <= t.xyHalf && p[2] >= t.zFloor;
}

auto IsSkyboxHidden(const SkyboxTransform& t, const float placed[3]) noexcept -> bool {
    return placed[0] >= t.worldMin[0] - t.hiddenMargin && placed[0] <= t.worldMax[0] + t.hiddenMargin &&
           placed[1] >= t.worldMin[1] - t.hiddenMargin && placed[1] <= t.worldMax[1] + t.hiddenMargin &&
           placed[2] >= t.worldMin[2] - t.hiddenMargin && placed[2] <= t.worldMax[2] + t.hiddenMargin;
}

void TransformSkyboxPoint(const SkyboxTransform& t, const float p[3], float out[3]) noexcept {
    const float v[3] = {(p[0] - t.origin[0]) * t.scale, (p[1] - t.origin[1]) * t.scale, (p[2] - t.origin[2]) * t.scale};
    out[0] = t.anchor[0] + t.rot[0] * v[0] + t.rot[1] * v[1] + t.rot[2] * v[2];
    out[1] = t.anchor[1] + t.rot[3] * v[0] + t.rot[4] * v[1] + t.rot[5] * v[2];
    out[2] = t.anchor[2] + t.rot[6] * v[0] + t.rot[7] * v[1] + t.rot[8] * v[2];
}

void TransformSkyboxVector(const SkyboxTransform& t, const float v[3], float out[3]) noexcept {
    out[0] = t.rot[0] * v[0] + t.rot[1] * v[1] + t.rot[2] * v[2];
    out[1] = t.rot[3] * v[0] + t.rot[4] * v[1] + t.rot[5] * v[2];
    out[2] = t.rot[6] * v[0] + t.rot[7] * v[1] + t.rot[8] * v[2];
}

void SkyboxMapPoint(const SkyboxTransform& t, const float p[3], float out[3]) noexcept {
    if (t.active && IsSkyboxPoint(t, p)) {
        TransformSkyboxPoint(t, p, out);
    } else {
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
    }
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

// The face walk ImportMapGeometry does inline, reduced to just the emissive
// records: winding, area, centroid, normal, material name, texdata
// reflectivity and the SURF_NOLIGHT flag. Kept in step with the inline version
// in ImportMapGeometry -- if one changes, the other must.
auto GatherEmissiveAreaLights(
    const BSPMap& map, const ImportOptions& options, std::span<const BspPointLight> entityLights
) -> std::vector<BspPointLight> {
    std::vector<BspPointLight> out;
    if (!options.gatherEmissiveSurfaces) {
        return out;
    }

    std::vector<EmissiveSurface> faces;
    const SkyboxTransform        sky = MakeSkyboxTransform(map);

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
                continue;
            }
            *winding = DropDuplicateCorners(*winding);
            if (winding->size() < 3) {
                continue;
            }
            if (sky.active) {
                const JPH::Vec3 c    = WindingCentroid(*winding);
                const float     src[3] = {c.GetX(), c.GetY(), c.GetZ()};
                if (IsSkyboxPoint(sky, src)) {
                    // Background scenery keeps its lamp entities; its panels do
                    // not compete for the play area's emissive-proxy budget.
                    continue;
                }
            }

            // Tool textures and compile helpers are not surfaces; only the
            // render stream can self-illuminate.
            if (DispositionFor(map, face) != FaceDisposition::Render) {
                continue;
            }

            const std::string_view materialName = MaterialNameFor(map, face);
            if (!IsEmissiveMaterialName(materialName)) {
                continue;
            }

            const double areaInches = WindingArea(*winding);
            if (areaInches <= 0.0) {
                continue;
            }

            EmissiveSurface surface;
            surface.materialName     = std::string(materialName);
            surface.position         = ConvertPosition(options, WindingCentroid(*winding));
            // The face's plane normal, +N out of the solid (see the winding
            // orientation note in ImportMapGeometry).
            JPH::Vec3 faceNormal = JPH::Vec3::sAxisZ();
            if (static_cast<size_t>(face.planenum) < map.planes.size()) {
                const DPlane& plane = map.planes[static_cast<size_t>(face.planenum)];
                faceNormal          = JPH::Vec3(plane.normal[0], plane.normal[1], plane.normal[2]);
            }
            surface.normal           = ConvertDirection(options, faceNormal);
            surface.areaSquareInches = static_cast<float>(areaInches);
            surface.areaSquareMetres = static_cast<float>(areaInches) * options.unitScale * options.unitScale;
            surface.faceCount        = 1;
            surface.reflectivityLuma = TexDataReflectivityLuma(map, face);
            surface.noLightFlag      = HasSurfNoLight(map, face);
            faces.push_back(std::move(surface));
        }
    }

    const std::vector<EmissiveSurface> proxies =
        FinalizeEmissiveSurfaces(faces, entityLights, options.unitScale, options.maxEmissiveLights);

    out.reserve(proxies.size());
    for (const EmissiveSurface& surface: proxies) {
        if (surface.strength <= 0.0f) {
            continue;
        }
        out.push_back(EmissiveLightFor(surface, options));
    }
    return out;
}

auto ImportMapGeometry(const BSPMap& map, const ImportOptions& options) -> ImportedMapData {
    ImportedMapData result;
    bool            globalBoundsFirst = true;

    // Filled in as the pass runs; EnforceLightingFloor refreshes it at the end.
    LightingDiagnostics diagnostics;

    // Post-pass input for the single-sheet wall audit (one record per
    // undisplaced render face; see ApplyTwinAudit).
    std::vector<TwinRecord> twinRecords;

    // Emissive-surface accumulator: one record per face the name gate admitted,
    // in engine space. Clustering, area weighting, fixture damping and the cap
    // all happen in BSPLighting::FinalizeEmissiveSurfaces, which needs the
    // per-face positions to do any of them -- collapsing to one record per
    // material here would put a single proxy at the map-wide centroid of every
    // panel that shares a name.
    std::vector<EmissiveSurface> emissiveFaces;

    // 3D skybox content is authored around the sky_camera at 1/scale size and
    // floats above the map until transformed; see SkyboxTransform.
    const SkyboxTransform sky = MakeSkyboxTransform(map);

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
            *winding = DropDuplicateCorners(*winding);
            if (winding->size() < 3) {
                ++result.stats.droppedDegenerate;
                continue;
            }

            // Skybox faces move from their compile cluster (around the
            // sky_camera, above the map) to their rendered world position
            // before any centroid/area work, so bake clusters, emissive
            // proxies and bounds all see the placed geometry. Nothing is
            // dropped: Source draws the whole miniscape and lets the world's
            // shell occlude it, which the renderer reproduces by compositing
            // these parts as a background layer behind the world's pixels.
            bool skyFace = false;
            if (sky.active) {
                const JPH::Vec3 centroid = WindingCentroid(*winding);
                const float    src[3] = {centroid.GetX(), centroid.GetY(), centroid.GetZ()};
                skyFace = IsSkyboxPoint(sky, src);
                if (skyFace) {
                    for (JPH::Vec3& corner: *winding) {
                        const float p[3] = {corner.GetX(), corner.GetY(), corner.GetZ()};
                        float       out[3];
                        TransformSkyboxPoint(sky, p, out);
                        corner = JPH::Vec3(out[0], out[1], out[2]);
                    }
                }
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
            if (skyFace) {
                const float n[3] = {faceNormal.GetX(), faceNormal.GetY(), faceNormal.GetZ()};
                float       out[3];
                TransformSkyboxVector(sky, n, out);
                faceNormal = JPH::Vec3(out[0], out[1], out[2]);
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
            // The skybox miniscape is scenery, not world collision: Source's
            // two-world split gives it no solid presence in the play space, so
            // its tool faces never join the collision hull either.
            if (skyFace && disposition == FaceDisposition::CollisionOnly) {
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


            const std::string_view materialName = MaterialNameFor(map, face);
            MaterialStreams& part = (disposition == FaceDisposition::CollisionOnly) ?
                                       result.collisionOnly :
                                       FindOrMakePart(result, std::string(materialName), skyFace);
            if (disposition == FaceDisposition::CollisionOnly && part.VertexCount() == 0) {
                part.materialName = std::string(materialName); // informational only; the importer does not key off it
            }

            // Self-illuminated surface accumulation. The geometry pass has no
            // VFS, so it only applies the name gate and records the face's
            // geometry; the importer adapter refines strength and tint from the
            // .vmt / .vtf and drops the candidates that clear no threshold
            // (BSPLighting::ProbeEmissiveMaterial).
            if (options.gatherEmissiveSurfaces && disposition == FaceDisposition::Render && IsEmissiveMaterialName(materialName) && !skyFace) {
                const double areaInches = WindingArea(*winding);
                if (areaInches > 0.0) {
                    const float areaMetres      = static_cast<float>(areaInches) * options.unitScale * options.unitScale;
                    EmissiveSurface surface;
                    surface.materialName       = std::string(materialName);
                    surface.position           = ConvertPosition(options, WindingCentroid(*winding));
                    surface.normal             = ConvertDirection(options, faceNormal);
                    surface.areaSquareInches   = static_cast<float>(areaInches);
                    surface.areaSquareMetres   = areaMetres;
                    surface.faceCount          = 1;
                    // Texdata reflectivity: a (1,1,1) panel is a lit white
                    // surface, not necessarily a lamp, and the probe uses that
                    // to damp the strength.
                    surface.reflectivityLuma   = TexDataReflectivityLuma(map, face);
                    // SURF_NOLIGHT is vbsp saying "do not bake light onto this
                    // surface" -- the only per-surface self-illumination bit in
                    // the whole .bsp, and the one that separates a real emitter
                    // from a bright albedo.
                    surface.noLightFlag        = HasSurfNoLight(map, face);
                    emissiveFaces.push_back(std::move(surface));
                }
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

                std::vector<JPH::Vec3> grid(static_cast<size_t>(side) * side);
                for (uint32_t j = 0; j < side; ++j) {
                    const float fv = static_cast<float>(j) / static_cast<float>(gridSize);
                    for (uint32_t i = 0; i < side; ++i) {
                        const float fu = static_cast<float>(i) / static_cast<float>(gridSize);

                        const JPH::Vec3 row0 = corners[0] + (corners[1] - corners[0]) * fu;
                        const JPH::Vec3 row1 = corners[3] + (corners[2] - corners[3]) * fu;
                        const JPH::Vec3 base = row0 + (row1 - row0) * fv;

                        const DDispVert& dispVert = map.dispVerts[vertBase + static_cast<size_t>(j) * side + i];
                        const JPH::Vec3  dispVec(dispVert.vec[0], dispVert.vec[1], dispVert.vec[2]);
                        grid[static_cast<size_t>(j) * side + i] = base + dispVec * dispVert.dist;
                    }
                }

                std::vector<JPH::Vec3> vertexNormals(static_cast<size_t>(side) * side, JPH::Vec3::sZero());
                for (uint32_t j = 0; j < gridSize; ++j) {
                    for (uint32_t i = 0; i < gridSize; ++i) {
                        const JPH::Vec3& p00 = grid[static_cast<size_t>(j) * side + i];
                        const JPH::Vec3& p10 = grid[static_cast<size_t>(j) * side + i + 1];
                        const JPH::Vec3& p01 = grid[static_cast<size_t>(j + 1) * side + i];
                        const JPH::Vec3 cell = (p10 - p00).Cross(p01 - p00);
                        for (uint32_t corner = 0; corner < 4; ++corner) {
                            const uint32_t ci = i + (corner & 1u);
                            const uint32_t cj = j + ((corner >> 1) & 1u);
                            vertexNormals[static_cast<size_t>(cj) * side + ci] += cell;
                        }
                    }
                }

                for (uint32_t j = 0; j < side; ++j) {
                    for (uint32_t i = 0; i < side; ++i) {
                        JPH::Vec3 vertexNormal = vertexNormals[static_cast<size_t>(j) * side + i];
                        if (vertexNormal.LengthSq() <= kEpsilon * kEpsilon) {
                            vertexNormal = faceNormal;
                        } else if (vertexNormal.Dot(faceNormal) < 0.0f) {
                            vertexNormal = -vertexNormal;
                        }
                        EmitVertex(part, options, *projection, grid[static_cast<size_t>(j) * side + i], vertexNormal, result, partBoundsFirst, globalBoundsFirst);
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
        result.lights = GatherEntityLights(map, options);
    }

    if (options.deriveAmbient) {
        result.ambient = ReadEnvironmentAmbient(map);
    }

    // Turn the per-material accumulator into clustered area-light proxies. The
    // strength is still zero here (this layer has no .vmt), so the proxies are
    // emitted at the family-name strength and the importer adapter re-probes
    // them with ProbeEmissiveMaterial, dropping the ones that turn out to be
    // ordinary lit surfaces. A `lights/white` sheet with reflectivity (1,1,1)
    // is exactly that case, which is why the probe exists rather than the name
    // gate being the last word.
    if (options.gatherEmissiveSurfaces && !emissiveFaces.empty()) {
        const size_t before    = result.lights.size();
        const size_t faceCount = emissiveFaces.size();
        result.emissive =
            FinalizeEmissiveSurfaces(emissiveFaces, result.lights, options.unitScale, options.maxEmissiveLights);

        // One point light per surviving proxy. These are the lights that carry
        // a self-illuminated surface into the PBR path, and the ones RTGI/DDGI
        // trace as virtual point lights -- the emissive VPLs PrefabFactory
        // spawns for glTF prefabs have no BSP-side equivalent, because a BSP
        // has no part hierarchy to walk.
        if (options.gatherLights) {
            for (const EmissiveSurface& surface: result.emissive) {
                if (surface.strength <= 0.0f) {
                    continue;
                }
                result.lights.push_back(EmissiveLightFor(surface, options));
            }
        }

        result.stats.emissiveSurfaces  = static_cast<uint32_t>(result.emissive.size());
        result.stats.emissiveFaces     = 0;
        float areaMetres               = 0.0f;
        uint32_t damped                = 0;
        for (const EmissiveSurface& surface: result.emissive) {
            result.stats.emissiveFaces += surface.faceCount;
            areaMetres += surface.areaSquareMetres;
            damped += (surface.fixtureDamping < 1.0f) ? 1u : 0u;
        }
        diagnostics.emissiveFaces           = result.stats.emissiveFaces;
        diagnostics.emissiveSurfaces        = result.stats.emissiveSurfaces;
        diagnostics.emissiveAreaSquareMetres = areaMetres;
        diagnostics.emissiveFixtureDamped   = damped;
        diagnostics.emissiveDroppedCap = (faceCount > result.emissive.size()) ? static_cast<uint32_t>(faceCount - result.emissive.size()) : 0u;
        diagnostics.totalEmissiveIntensity = 0.0f;
        for (size_t i = before; i < result.lights.size(); ++i) {
            diagnostics.totalEmissiveIntensity += result.lights[i].intensity;
        }
    }

    // ------------------------------------------------------------------
    // VRAD bake ingestion: fit LUMP_LIGHTING's radiance field into the
    // engine's carriers (BSPBakeField.hpp). The fit lands before the floor
    // guarantee so a map lighted only by its bake is still a lit map.
    // ------------------------------------------------------------------
    if (options.ingestVradBake && options.gatherLights && !map.lighting.empty()) {
        const BakeField field = BuildBakeField(map, options, options.bakeVplBudget, options.bakeVplWeight);
        if (field.hasBake) {
            result.ambient      = field.ambient;
            result.stats.bakeLuxels = field.sampleCount;
            for (const BspPointLight& vpl: field.vpls) {
                result.lights.push_back(vpl);
            }
            result.stats.bakeVpls = static_cast<uint32_t>(field.vpls.size());
        }
    }

    // The guarantee runs last so it sees the final light set, and the
    // diagnostics travel with the imported data so the importer can log them.
    diagnostics.floorEnforced = EnforceLightingFloor(result, map, options, diagnostics);
    result.lighting           = diagnostics;
    return result;
}

} // namespace ZHLN::BSP
