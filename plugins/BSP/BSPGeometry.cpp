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
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace ZHLN::BSP {
namespace {

constexpr float kEpsilon = 1e-6f;

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

auto ParseColor4(std::string_view text, float out[4]) -> bool {
    const std::string buffer(text);
    const char*       cursor = buffer.c_str();
    char*             end    = nullptr;
    for (size_t k = 0; k < 4; ++k) {
        out[k] = std::strtof(cursor, &end);
        if (end == cursor) {
            return false;
        }
        cursor = end;
    }
    return true;
}

// The closed vertex loop of one face, in face winding order, source space.
auto FaceWinding(const BSPMap& map, const DFace& face, std::vector<JPH::Vec3>& out) -> bool {
    if (face.firstedge < 0 || face.numedges < 3) {
        return false;
    }
    if (static_cast<size_t>(face.firstedge) + static_cast<size_t>(face.numedges) > map.surfEdges.size()) {
        return false;
    }
    out.clear();
    out.reserve(static_cast<size_t>(face.numedges));
    for (int16_t k = 0; k < face.numedges; ++k) {
        const int32_t surfEdge  = map.surfEdges[static_cast<size_t>(face.firstedge) + static_cast<size_t>(k)];
        const int64_t edgeIndex = (surfEdge >= 0) ? surfEdge : -static_cast<int64_t>(surfEdge);
        if (edgeIndex < 0 || static_cast<size_t>(edgeIndex) >= map.edges.size()) {
            return false;
        }
        const DEdge&   edge = map.edges[static_cast<size_t>(edgeIndex)];
        const uint16_t vert = (surfEdge >= 0) ? edge.v[0] : edge.v[1];
        if (vert >= map.vertices.size()) {
            return false;
        }
        const auto& pt = map.vertices[vert].point;
        out.emplace_back(pt[0], pt[1], pt[2]);
    }
    return true;
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
};

auto ProjectionFor(const BSPMap& map, const DFace& face, FaceProjection& out) -> bool {
    if (face.texinfo == kTexInfoNode || face.texinfo < 0 || static_cast<size_t>(face.texinfo) >= map.texInfos.size()) {
        return false;
    }
    const DTexInfo& texInfo = map.texInfos[static_cast<size_t>(face.texinfo)];
    if (texInfo.texdata < 0 || static_cast<size_t>(texInfo.texdata) >= map.texDatas.size()) {
        return false;
    }
    const DTexData& texData = map.texDatas[static_cast<size_t>(texInfo.texdata)];
    out.sDir                = JPH::Vec3(texInfo.textureVecs[0][0], texInfo.textureVecs[0][1], texInfo.textureVecs[0][2]);
    out.sOffset             = texInfo.textureVecs[0][3];
    out.tDir                = JPH::Vec3(texInfo.textureVecs[1][0], texInfo.textureVecs[1][1], texInfo.textureVecs[1][2]);
    out.tOffset             = texInfo.textureVecs[1][3];
    out.width               = (texData.width > 0) ? static_cast<float>(texData.width) : 512.0f;
    out.height              = (texData.height > 0) ? static_cast<float>(texData.height) : 512.0f;
    return true;
}

void MaterialNameFor(const BSPMap& map, const DFace& face, std::string& out) {
    out = "unknown";
    if (face.texinfo < 0 || static_cast<size_t>(face.texinfo) >= map.texInfos.size()) {
        return;
    }
    const int32_t texDataIndex = map.texInfos[static_cast<size_t>(face.texinfo)].texdata;
    if (texDataIndex < 0 || static_cast<size_t>(texDataIndex) >= map.texNames.size()) {
        return;
    }
    out = map.texNames[static_cast<size_t>(texDataIndex)];
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
    part.surfaces.push_back(VertexSurface {Math::PackUV(u, v), Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f), Math::PackUV(0.0f, 0.0f)});

    UpdateBounds(part.boundsMin, part.boundsMax, partBoundsFirst, enginePos);
    UpdateBounds(result.boundsMin, result.boundsMax, mapBoundsFirst, enginePos);
}

void GatherLights(const BSPMap& map, const ImportOptions& options, std::vector<BspPointLight>& out) {
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

        float originArr[3] = {0.0f, 0.0f, 0.0f};
        static_cast<void>(entity.FindVector("origin", originArr));
        light.position = ConvertPosition(options, JPH::Vec3(originArr[0], originArr[1], originArr[2]));

        float color4[4] = {255.0f, 255.0f, 255.0f, 200.0f};
        if (!entity.Find("_light").empty()) {
            ParseColor4(entity.Find("_light"), color4);
        }
        light.color =
            JPH::Vec3(std::clamp(color4[0] / 255.0f, 0.0f, 1.0f), std::clamp(color4[1] / 255.0f, 0.0f, 1.0f), std::clamp(color4[2] / 255.0f, 0.0f, 1.0f));
        light.intensity = color4[3];

        float anglesArr[3] = {0.0f, 0.0f, 0.0f};
        if (!entity.Find("angles").empty()) {
            static_cast<void>(entity.FindVector("angles", anglesArr));
        } else if (!entity.Find("pitch").empty()) {
            // light_environment keeps its aim in "pitch" and "angles"(yaw).
            static_cast<void>(entity.FindVector("angles", anglesArr));
            anglesArr[0] = std::strtof(std::string(entity.Find("pitch")).c_str(), nullptr);
        }
        const JPH::Vec3 direction = AngleDirection(JPH::Vec3(anglesArr[0], anglesArr[1], anglesArr[2]));
        light.direction           = ConvertDirection(options, direction);

        if (type == LightType::Spot) {
            const float outerDegrees = std::strtof(std::string(entity.Find("_cone")).c_str(), nullptr);
            const float innerDegrees = std::strtof(std::string(entity.Find("_cone2")).c_str(), nullptr);
            if (outerDegrees > 0.0f) {
                light.outerConeRadians = outerDegrees * (3.14159265358979323846f / 180.0f);
            }
            if (innerDegrees > 0.0f) {
                light.innerConeRadians = innerDegrees * (3.14159265358979323846f / 180.0f);
            }
        }

        out.push_back(std::move(light));
    }
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

    for (const DModel& model: map.models) {
        if (model.firstface < 0 || model.numfaces < 0) {
            continue;
        }
        const size_t faceEnd = std::min(static_cast<size_t>(model.firstface) + static_cast<size_t>(model.numfaces), map.faces.size());
        for (size_t faceIndex = static_cast<size_t>(model.firstface); faceIndex < faceEnd; ++faceIndex) {
            const DFace& face = map.faces[faceIndex];

            std::vector<JPH::Vec3> winding;
            if (!FaceWinding(map, face, winding)) {
                continue;
            }
            DropDuplicateCorners(winding);
            if (winding.size() < 3) {
                continue;
            }

            FaceProjection projection;
            if (!ProjectionFor(map, face, projection)) {
                continue;
            }

            std::string materialName;
            MaterialNameFor(map, face, materialName);
            MaterialStreams& part = FindOrMakePart(result, materialName);

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

            const bool displaced = options.includeDisplacements && face.dispinfo >= 0 && static_cast<size_t>(face.dispinfo) < map.dispInfos.size() &&
                                   winding.size() == 4;

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
                    const float distSq = (winding[c] - dispStartPos).LengthSq();
                    if (distSq < bestDistSq) {
                        bestDistSq = distSq;
                        startIndex = c;
                    }
                }
                const std::array<JPH::Vec3, 4> corners = {
                    winding[startIndex],
                    winding[(startIndex + 1) % 4],
                    winding[(startIndex + 2) % 4],
                    winding[(startIndex + 3) % 4],
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

                        EmitVertex(part, options, projection, sourcePos, faceNormal, result, partBoundsFirst, globalBoundsFirst);
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
                ++result.displacementCount;
                result.triangleCount += gridSize * gridSize * 2;
            } else {
                // Fan triangulation: Source faces are convex planar polygons.
                for (const auto& corner: winding) {
                    EmitVertex(part, options, projection, corner, faceNormal, result, partBoundsFirst, globalBoundsFirst);
                }
                for (size_t i = 1; i + 1 < winding.size(); ++i) {
                    part.indices.push_back(baseVertex);
                    part.indices.push_back(baseVertex + static_cast<uint32_t>(i));
                    part.indices.push_back(baseVertex + static_cast<uint32_t>(i) + 1u);
                }
                result.triangleCount += static_cast<uint32_t>(winding.size()) - 2;
            }

            ++result.faceCount;
        }
    }

    if (options.gatherLights) {
        GatherLights(map, options, result.lights);
    }
    return result;
}

} // namespace ZHLN::BSP
