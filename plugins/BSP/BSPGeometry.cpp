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
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string_view>

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
    out.sDir                = JPH::Vec3(texInfo.textureVecs[0][0], texInfo.textureVecs[0][1], texInfo.textureVecs[0][2]);
    out.sOffset             = texInfo.textureVecs[0][3];
    out.tDir                = JPH::Vec3(texInfo.textureVecs[1][0], texInfo.textureVecs[1][1], texInfo.textureVecs[1][2]);
    out.tOffset             = texInfo.textureVecs[1][3];
    out.width               = (texData.width > 0) ? static_cast<float>(texData.width) : 512.0f;
    out.height              = (texData.height > 0) ? static_cast<float>(texData.height) : 512.0f;

    out.lightmapSDir        = JPH::Vec3(texInfo.lightmapVecs[0][0], texInfo.lightmapVecs[0][1], texInfo.lightmapVecs[0][2]);
    out.lightmapSOffset     = texInfo.lightmapVecs[0][3];
    out.lightmapTDir        = JPH::Vec3(texInfo.lightmapVecs[1][0], texInfo.lightmapVecs[1][1], texInfo.lightmapVecs[1][2]);
    out.lightmapTOffset     = texInfo.lightmapVecs[1][3];
    out.lightmapMins[0]     = face.lightmapTextureMinsInLuxels[0];
    out.lightmapMins[1]     = face.lightmapTextureMinsInLuxels[1];
    out.lightmapSize[0]     = face.lightmapTextureSizeInLuxels[0];
    out.lightmapSize[1]     = face.lightmapTextureSizeInLuxels[1];
    out.hasLightmap         = (face.lightmapTextureSizeInLuxels[0] > 0 && face.lightmapTextureSizeInLuxels[1] > 0);
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
        lmU = (result.lightmapPageWidth > 0.0f) ? std::clamp((sLuxel + 0.5f) / result.lightmapPageWidth, 0.0f, 1.0f) : 0.0f;
        lmV = (result.lightmapPageHeight > 0.0f) ? std::clamp((tLuxel + 0.5f) / result.lightmapPageHeight, 0.0f, 1.0f) : 0.0f;
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
    part.tangentFrames.push_back(VertexTangentFrame {
        Math::PackNormal(engineNormal.GetX(), engineNormal.GetY(), engineNormal.GetZ()),
        Math::PackNormal(engineTangent.GetX(), engineTangent.GetY(), engineTangent.GetZ(), handedness),
    });
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
        light.intensity = color4[3];

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

auto ImportMapGeometry(const BSPMap& map, const ImportOptions& options) -> ImportedMapData {
    ImportedMapData result;
    bool            globalBoundsFirst = true;

    // Lightmap atlas extent: baked lightmap pages (the *_lightmap0.png dumps
    // that community tools produce) place each face's sample rect at its
    // lightmap mins inside one shared page, so vertex lightmap UVs must be in
    // that page's coordinate space. The page is max(mins + allocated samples)
    // across faces; Source reserves size+1 luxels per axis per face.
    for (const DFace& face: map.faces) {
        if (face.lightmapTextureSizeInLuxels[0] <= 0 || face.lightmapTextureSizeInLuxels[1] <= 0) {
            continue;
        }
        result.lightmapPageWidth =
            std::max(result.lightmapPageWidth, static_cast<float>(face.lightmapTextureMinsInLuxels[0] + face.lightmapTextureSizeInLuxels[0] + 1));
        result.lightmapPageHeight =
            std::max(result.lightmapPageHeight, static_cast<float>(face.lightmapTextureMinsInLuxels[1] + face.lightmapTextureSizeInLuxels[1] + 1));
    }

    for (const DModel& model: map.models) {
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

            // Faces on the back side of their plane (side != 0) store their
            // winding reversed in the file: the loop is CCW about the plane's
            // normal, opposite to the face's own normal. Emit vertices in the
            // face-normal orientation instead, keeping triangle fronts
            // consistent with the flipped faceNormal below. This one flip is
            // what both consumers key off: the renderer culls back faces (an
            // unreversed interior wall is culled away and invisible from
            // inside), and the world Jolt mesh collider is built from these
            // same triangles (rigid bodies resting on an unreversed floor
            // contact only its front side and drop straight through; the
            // CharacterVirtual walks anywhere only because it opts into
            // EBackFaceMode::CollideWithBackFaces).
            if (face.side != 0) {
                std::reverse(winding->begin(), winding->end());
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

            const auto projection = ProjectionFor(map, face);
            if (!projection) {
                ++result.stats.droppedNoProjection;
                continue;
            }

            const std::string_view materialName = MaterialNameFor(map, face);
            MaterialStreams&       part = (disposition == FaceDisposition::CollisionOnly) ? result.collisionOnly : FindOrMakePart(result, std::string(materialName));
            if (disposition == FaceDisposition::CollisionOnly && part.VertexCount() == 0) {
                part.materialName = std::string(materialName); // informational only; the importer does not key off it
            }

            JPH::Vec3 faceNormal = JPH::Vec3::sAxisZ();
            if (static_cast<size_t>(face.planenum) < map.planes.size()) {
                const DPlane& plane = map.planes[static_cast<size_t>(face.planenum)];
                faceNormal          = JPH::Vec3(plane.normal[0], plane.normal[1], plane.normal[2]);
                if (face.side != 0) {
                    faceNormal = -faceNormal;
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
                const DDispInfo& dispInfo = map.dispInfos[static_cast<size_t>(face.dispinfo)];
                const int32_t    power    = dispInfo.power;
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

            if (disposition != FaceDisposition::CollisionOnly) {
                ++result.faceCount;
            } else {
                ++result.stats.collisionOnlyFaces;
            }
        }
    }

    if (options.gatherLights) {
        result.lights = GatherLights(map, options);
    }
    return result;
}

} // namespace ZHLN::BSP
