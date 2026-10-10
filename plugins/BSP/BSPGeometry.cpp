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
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace ZHLN::BSP {
namespace {

constexpr float kEpsilon = 1e-6f;

auto Dot(const float a[3], const float b[3]) -> float {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void Cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

auto Length(const float v[3]) -> float {
    return std::sqrt(Dot(v, v));
}

void Normalize(float v[3]) {
    const float len = Length(v);
    if (len > kEpsilon) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

void Lerp3(const float a[3], const float b[3], float t, float out[3]) {
    out[0] = a[0] + (b[0] - a[0]) * t;
    out[1] = a[1] + (b[1] - a[1]) * t;
    out[2] = a[2] + (b[2] - a[2]) * t;
}

void MinMax(float boundsMin[3], float boundsMax[3], bool& first, const float p[3]) {
    if (first) {
        for (int k = 0; k < 3; ++k) {
            boundsMin[k] = p[k];
            boundsMax[k] = p[k];
        }
        first = false;
        return;
    }
    for (int k = 0; k < 3; ++k) {
        boundsMin[k] = std::min(boundsMin[k], p[k]);
        boundsMax[k] = std::max(boundsMax[k], p[k]);
    }
}

// Source angle triples are (pitch, yaw, roll) degrees, pitch positive down,
// around a Z-up world. Pitch/yaw -> unit direction in source space.
void AngleDirection(const float angles[3], float out[3]) {
    const float pitch = angles[0] * (3.14159265358979323846f / 180.0f);
    const float yaw   = angles[1] * (3.14159265358979323846f / 180.0f);
    const float cp    = std::cos(pitch);
    out[0]            = cp * std::cos(yaw);
    out[1]            = cp * std::sin(yaw);
    out[2]            = -std::sin(pitch);
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
auto FaceWinding(const BSPMap& map, const DFace& face, std::vector<std::array<float, 3>>& out) -> bool {
    if (face.firstedge < 0 || face.numedges < 3) {
        return false;
    }
    if (static_cast<size_t>(face.firstedge) + static_cast<size_t>(face.numedges) > map.surfEdges.size()) {
        return false;
    }
    out.clear();
    out.reserve(static_cast<size_t>(face.numedges));
    for (int16_t k = 0; k < face.numedges; ++k) {
        const int32_t surfEdge = map.surfEdges[static_cast<size_t>(face.firstedge) + static_cast<size_t>(k)];
        const int64_t edgeIndex = (surfEdge >= 0) ? surfEdge : -static_cast<int64_t>(surfEdge);
        if (edgeIndex < 0 || static_cast<size_t>(edgeIndex) >= map.edges.size()) {
            return false;
        }
        const DEdge& edge   = map.edges[static_cast<size_t>(edgeIndex)];
        const uint16_t vert = (surfEdge >= 0) ? edge.v[0] : edge.v[1];
        if (vert >= map.vertices.size()) {
            return false;
        }
        out.push_back({map.vertices[vert].point[0], map.vertices[vert].point[1], map.vertices[vert].point[2]});
    }
    return true;
}

void DropDuplicateCorners(std::vector<std::array<float, 3>>& winding) {
    std::vector<std::array<float, 3>> unique;
    unique.reserve(winding.size());
    for (const auto& point: winding) {
        if (unique.empty() || std::abs(point[0] - unique.back()[0]) > kEpsilon || std::abs(point[1] - unique.back()[1]) > kEpsilon ||
            std::abs(point[2] - unique.back()[2]) > kEpsilon) {
            unique.push_back(point);
        }
    }
    if (unique.size() > 1) {
        const auto& first = unique.front();
        const auto& last  = unique.back();
        if (std::abs(first[0] - last[0]) <= kEpsilon && std::abs(first[1] - last[1]) <= kEpsilon && std::abs(first[2] - last[2]) <= kEpsilon) {
            unique.pop_back();
        }
    }
    winding = std::move(unique);
}

struct FaceProjection {
    float s[4] = {1, 0, 0, 0};
    float t[4] = {0, 1, 0, 0};
    float width  = 512.0f;
    float height = 512.0f;
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
    for (int k = 0; k < 4; ++k) {
        out.s[k] = texInfo.textureVecs[0][k];
        out.t[k] = texInfo.textureVecs[1][k];
    }
    out.width  = (texData.width > 0) ? static_cast<float>(texData.width) : 512.0f;
    out.height = (texData.height > 0) ? static_cast<float>(texData.height) : 512.0f;
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

auto FindOrMakePart(MarshalledMap& result, const std::string& name) -> MaterialStreams& {
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
    MaterialStreams&       part,
    const MarshallOptions& options,
    const FaceProjection&  projection,
    const float            sourcePos[3],
    const float            sourceNormal[3],
    MarshalledMap&         result,
    bool&                  partBoundsFirst,
    bool&                  mapBoundsFirst
) {
    const float u = (Dot(sourcePos, projection.s) + projection.s[3]) / projection.width;
    const float v = (Dot(sourcePos, projection.t) + projection.t[3]) / projection.height;

    float tangent[3] = {projection.s[0], projection.s[1], projection.s[2]};
    Normalize(tangent);

    float normal[3] = {sourceNormal[0], sourceNormal[1], sourceNormal[2]};
    Normalize(normal);

    // Handedness so that bitangent = cross(normal, tangent.xyz) * tangent.w,
    // which is the glTF convention the renderer's tangent frames assume.
    float crossNT[3];
    Cross(normal, tangent, crossNT);
    const float handedness = (Dot(crossNT, projection.t) < 0.0f) ? -1.0f : 1.0f;

    float enginePos[3];
    float engineNormal[3];
    float engineTangent[3];
    ConvertPosition(options, sourcePos, enginePos);
    ConvertDirection(options, normal, engineNormal);
    ConvertDirection(options, tangent, engineTangent);

    part.positions.push_back(VertexPosition {{enginePos[0], enginePos[1], enginePos[2]}});
    part.tangentFrames.push_back(VertexTangentFrame {
        Math::PackNormal(engineNormal[0], engineNormal[1], engineNormal[2]),
        Math::PackNormal(engineTangent[0], engineTangent[1], engineTangent[2], handedness),
    });
    part.surfaces.push_back(VertexSurface {Math::PackUV(u, v), Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f), Math::PackUV(0.0f, 0.0f)});

    MinMax(part.boundsMin, part.boundsMax, partBoundsFirst, enginePos);
    MinMax(result.boundsMin, result.boundsMax, mapBoundsFirst, enginePos);
}

void GatherLights(const BSPMap& map, const MarshallOptions& options, std::vector<BspPointLight>& out) {
    for (const BSPEntity& entity: map.entities) {
        const std::string_view className = entity.Find("classname");
        BspPointLight::Kind    kind;
        if (className == "light") {
            kind = BspPointLight::Kind::Point;
        } else if (className == "light_spot") {
            kind = BspPointLight::Kind::Spot;
        } else if (className == "light_environment") {
            kind = BspPointLight::Kind::Directional;
        } else {
            continue;
        }

        BspPointLight light;
        light.kind = kind;
        light.name = std::string(entity.Find("targetname"));
        if (light.name.empty()) {
            light.name = std::string(className);
        }

        float origin[3] = {0.0f, 0.0f, 0.0f};
        static_cast<void>(entity.FindVector("origin", origin));
        ConvertPosition(options, origin, light.position);

        float color4[4] = {255.0f, 255.0f, 255.0f, 200.0f};
        if (entity.Has("_light")) {
            ParseColor4(entity.Find("_light"), color4);
        }
        for (int k = 0; k < 3; ++k) {
            light.color[k] = std::clamp(color4[k] / 255.0f, 0.0f, 1.0f);
        }
        light.intensity = color4[3];

        float angles[3] = {0.0f, 0.0f, 0.0f};
        if (entity.Has("angles")) {
            static_cast<void>(entity.FindVector("angles", angles));
        } else if (entity.Has("pitch")) {
            // light_environment keeps its aim in "pitch" and "angles"(yaw).
            static_cast<void>(entity.FindVector("angles", angles));
            angles[0] = std::strtof(std::string(entity.Find("pitch")).c_str(), nullptr);
        }
        float direction[3] = {0.0f, 0.0f, -1.0f};
        AngleDirection(angles, direction);
        ConvertDirection(options, direction, light.direction);

        if (kind == BspPointLight::Kind::Spot) {
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

void ConvertPosition(const MarshallOptions& options, const float in[3], float out[3]) noexcept {
    const float s = options.unitScale;
    if (!options.convertCoordinates) {
        out[0] = in[0] * s;
        out[1] = in[1] * s;
        out[2] = in[2] * s;
        return;
    }
    // Z-up right-handed -> Y-up right-handed: rotate -90 degrees about X,
    // then scale. (x, y, z) -> (x, z, -y).
    out[0] = in[0] * s;
    out[1] = in[2] * s;
    out[2] = -in[1] * s;
}

void ConvertDirection(const MarshallOptions& options, const float in[3], float out[3]) noexcept {
    if (!options.convertCoordinates) {
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
        return;
    }
    out[0] = in[0];
    out[1] = in[2];
    out[2] = -in[1];
}

auto MarshallMap(const BSPMap& map, const MarshallOptions& options) -> MarshalledMap {
    MarshalledMap result;
    bool          globalBoundsFirst = true;

    for (const DModel& model: map.models) {
        if (model.firstface < 0 || model.numfaces < 0) {
            continue;
        }
        const size_t faceEnd = std::min(static_cast<size_t>(model.firstface) + static_cast<size_t>(model.numfaces), map.faces.size());
        for (size_t faceIndex = static_cast<size_t>(model.firstface); faceIndex < faceEnd; ++faceIndex) {
            const DFace& face = map.faces[faceIndex];

            std::vector<std::array<float, 3>> winding;
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

            float faceNormal[3] = {0.0f, 0.0f, 1.0f};
            if (face.planenum >= 0 && static_cast<size_t>(face.planenum) < map.planes.size()) {
                const DPlane& plane = map.planes[static_cast<size_t>(face.planenum)];
                faceNormal[0]       = plane.normal[0];
                faceNormal[1]       = plane.normal[1];
                faceNormal[2]       = plane.normal[2];
                if (face.side != 0) {
                    faceNormal[0] = -faceNormal[0];
                    faceNormal[1] = -faceNormal[1];
                    faceNormal[2] = -faceNormal[2];
                }
            }

            const uint32_t baseVertex = part.VertexCount();
            bool           partBoundsFirst = (baseVertex == 0);

            const bool displaced = options.includeDisplacements && face.dispinfo >= 0 &&
                                   static_cast<size_t>(face.dispinfo) < map.dispInfos.size() && winding.size() == 4;

            if (displaced) {
                const DDispInfo& dispInfo = map.dispInfos[static_cast<size_t>(face.dispinfo)];
                const int32_t    power    = dispInfo.power;
                if (power < 1 || power > 4) {
                    continue;
                }
                const uint32_t gridSize = 1u << static_cast<uint32_t>(power);
                const uint32_t side     = gridSize + 1;

                // Rotate the base quad so corner 0 is the displacement's start
                // position, keeping winding order.
                size_t startIndex = 0;
                float  bestDist   = 1e30f;
                for (size_t c = 0; c < 4; ++c) {
                    float d[3] = {winding[c][0] - dispInfo.startPosition[0], winding[c][1] - dispInfo.startPosition[1], winding[c][2] - dispInfo.startPosition[2]};
                    const float dist = Dot(d, d);
                    if (dist < bestDist) {
                        bestDist   = dist;
                        startIndex = c;
                    }
                }
                const std::array<float, 3> corners[4] = {
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

                        float row[3];
                        float base[3];
                        Lerp3(corners[0].data(), corners[1].data(), fu, row);
                        Lerp3(corners[3].data(), corners[2].data(), fu, base);
                        Lerp3(row, base, fv, base);

                        const DDispVert& dispVert = map.dispVerts[vertBase + static_cast<size_t>(j) * side + i];
                        float            sourcePos[3] = {
                            base[0] + dispVert.vec[0] * dispVert.dist,
                            base[1] + dispVert.vec[1] * dispVert.dist,
                            base[2] + dispVert.vec[2] * dispVert.dist,
                        };
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
                    const float sourcePos[3] = {corner[0], corner[1], corner[2]};
                    EmitVertex(part, options, projection, sourcePos, faceNormal, result, partBoundsFirst, globalBoundsFirst);
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
