// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/helpers/ChamferedBoxMesh.hpp
//
// Authored chamfered-box mesh shared by the unlit test fixtures.
//
// Replaces the Khronos UnlitTest.glb sample asset (CC-BY 4.0, (c) 2019
// Analytical Graphics, Inc. -- Ed Mackey) with generated C++ so the suites do
// not depend on a binary blob: same solid (faces at +/-1, chamfer inset
// 0.66667, orange/blue objects at X = -/+1.2), same 96 vertices and 132
// indices, flat face normals. Vertex ORDER differs from the sample --
// rasterization of opaque single-sided geometry does not depend on it.
//
// Deliberately dependency-free: no engine headers, no reflection, no device.
// The standalone harness below verifies winding, closure and geometric
// equivalence against the retired sample; run it after touching this file:
//
//   g++ -std=c++23 -Wall -Wextra -Werror -I tests/helpers /tmp/chamfer_dump.cpp

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ZHLN::Test::GltfFixtures {

struct ChamferedBoxMesh {
    static constexpr uint32_t kVertexCount   = 96;
    static constexpr uint32_t kIndexCount    = 132;
    static constexpr uint32_t kPositionBytes = kVertexCount * 3 * sizeof(float);
    static constexpr uint32_t kNormalBytes   = kVertexCount * 3 * sizeof(float);
    static constexpr uint32_t kIndexBytes    = kIndexCount * sizeof(uint16_t);
    static constexpr uint32_t kTotalBytes    = kPositionBytes + kNormalBytes + kIndexBytes;
    std::array<float, kVertexCount * 3>      positions {};
    std::array<float, kVertexCount * 3>      normals {};
    std::array<uint16_t, kIndexCount>        indices {};
};

static_assert(sizeof(ChamferedBoxMesh::positions) == ChamferedBoxMesh::kPositionBytes);
static_assert(sizeof(ChamferedBoxMesh::normals) == ChamferedBoxMesh::kNormalBytes);
static_assert(sizeof(ChamferedBoxMesh::indices) == ChamferedBoxMesh::kIndexBytes);
static_assert(ChamferedBoxMesh::kTotalBytes == 2568);

// Chamfered cube: 6 face quads, 12 edge chamfers, 8 corner cuts. Every vertex
// is split per polygon for flat normals: 24 + 48 + 24 vertices, 44 triangles.
// The polygon emitter is nested privately: it exists only for this builder.
struct ChamferedBoxBuilder {
    [[nodiscard]] static auto Build() -> ChamferedBoxMesh {
        constexpr float kHalf    = 1.0f;
        constexpr float kChamfer = 0.66667f;
        const float     kEdge    = 1.0f / std::sqrt(2.0f);
        const float     kCorner  = 1.0f / std::sqrt(3.0f);

        ChamferedBoxMesh mesh {};
        PolygonEmitter   emitter {mesh};

        for (uint32_t axis = 0; axis < 3; ++axis) {
            for (const float sign: {-1.0f, 1.0f}) {
                const uint32_t u = (axis + 1) % 3;
                const uint32_t v = (axis + 2) % 3;
                std::vector<Vec3> corners;
                for (const float su: {-1.0f, 1.0f}) {
                    for (const float sv: {-1.0f, 1.0f}) {
                        Vec3 corner {};
                        SetAxis(corner, axis, sign * kHalf);
                        SetAxis(corner, u, su * kChamfer);
                        SetAxis(corner, v, sv * kChamfer);
                        corners.push_back(corner);
                    }
                }
                Vec3 normal {};
                SetAxis(normal, axis, sign);
                emitter.Emit(normal, corners);
            }
        }

        for (uint32_t pair = 0; pair < 3; ++pair) {
            const uint32_t a = (pair + 1) % 3;
            const uint32_t b = (pair + 2) % 3;
            const uint32_t t = pair;
            for (const float sa: {-1.0f, 1.0f}) {
                for (const float sb: {-1.0f, 1.0f}) {
                    std::vector<Vec3> corners;
                    for (const float st: {-1.0f, 1.0f}) {
                        for (uint32_t end = 0; end < 2; ++end) {
                            Vec3 corner {};
                            SetAxis(corner, t, st * kChamfer);
                            SetAxis(corner, a, (end == 0) ? sa * kHalf : sa * kChamfer);
                            SetAxis(corner, b, (end == 0) ? sb * kChamfer : sb * kHalf);
                            corners.push_back(corner);
                        }
                    }
                    Vec3 normal {};
                    SetAxis(normal, a, sa * kEdge);
                    SetAxis(normal, b, sb * kEdge);
                    emitter.Emit(normal, corners);
                }
            }
        }

        for (const float sx: {-1.0f, 1.0f}) {
            for (const float sy: {-1.0f, 1.0f}) {
                for (const float sz: {-1.0f, 1.0f}) {
                    // One full coordinate per vertex: the cut meets the three
                    // edge chamfers, e.g. (+1,+c,+c), (+c,+1,+c), (+c,+c,+1).
                    emitter.Emit(
                        {sx * kCorner, sy * kCorner, sz * kCorner},
                        {
                            {sx * kHalf, sy * kChamfer, sz * kChamfer},
                            {sx * kChamfer, sy * kHalf, sz * kChamfer},
                            {sx * kChamfer, sy * kChamfer, sz * kHalf},
                        }
                    );
                }
            }
        }

        return mesh;
    }

private:
    struct Vec3 {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    static void SetAxis(Vec3& v, uint32_t axis, float value) noexcept {
        if (axis == 0) v.x = value;
        else if (axis == 1) v.y = value;
        else v.z = value;
    }

    static constexpr auto Sub(Vec3 a, Vec3 b) noexcept -> Vec3 {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }

    static constexpr auto Cross(Vec3 a, Vec3 b) noexcept -> Vec3 {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

    static constexpr auto Dot(Vec3 a, Vec3 b) noexcept -> float {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static auto Normalize(Vec3 v) noexcept -> Vec3 {
        const float length = std::sqrt(Dot(v, v));
        return {v.x / length, v.y / length, v.z / length};
    }

    struct PolygonEmitter {
        ChamferedBoxMesh& mesh;
        uint32_t          vertexCursor = 0;
        uint32_t          indexCursor  = 0;

        // Appends one convex polygon with a flat normal. Corners are ordered
        // by angle around the normal, so the fan triangulation is
        // counter-clockwise seen from outside whatever order the caller
        // enumerated them in.
        void Emit(const Vec3& normal, const std::vector<Vec3>& corners) {
            Vec3 centroid {};
            for (const Vec3& corner: corners) {
                centroid.x += corner.x;
                centroid.y += corner.y;
                centroid.z += corner.z;
            }
            const float count = static_cast<float>(corners.size());
            centroid          = {centroid.x / count, centroid.y / count, centroid.z / count};

            const Vec3 helper    = (std::abs(normal.x) < 0.9f) ? Vec3 {1.0f, 0.0f, 0.0f} : Vec3 {0.0f, 1.0f, 0.0f};
            const Vec3 tangent   = Normalize(Cross(normal, helper));
            const Vec3 bitangent = Cross(normal, tangent);

            std::vector<Vec3> ordered = corners;
            for (size_t i = 1; i < ordered.size(); ++i) {
                const Vec3  key       = ordered[i];
                const Vec3  keyDelta  = Sub(key, centroid);
                const float keyAngle  = std::atan2(Dot(keyDelta, bitangent), Dot(keyDelta, tangent));
                size_t      j         = i;
                while (j > 0) {
                    const Vec3  prevDelta  = Sub(ordered[j - 1], centroid);
                    const float prevAngle  = std::atan2(Dot(prevDelta, bitangent), Dot(prevDelta, tangent));
                    if (prevAngle <= keyAngle) break;
                    ordered[j] = ordered[j - 1];
                    --j;
                }
                ordered[j] = key;
            }

            const uint32_t first = vertexCursor;
            for (const Vec3& corner: ordered) {
                mesh.positions[vertexCursor * 3 + 0] = corner.x;
                mesh.positions[vertexCursor * 3 + 1] = corner.y;
                mesh.positions[vertexCursor * 3 + 2] = corner.z;
                mesh.normals[vertexCursor * 3 + 0]   = normal.x;
                mesh.normals[vertexCursor * 3 + 1]   = normal.y;
                mesh.normals[vertexCursor * 3 + 2]   = normal.z;
                ++vertexCursor;
            }
            for (size_t i = 1; i + 1 < ordered.size(); ++i) {
                mesh.indices[indexCursor++] = static_cast<uint16_t>(first);
                mesh.indices[indexCursor++] = static_cast<uint16_t>(first + i);
                mesh.indices[indexCursor++] = static_cast<uint16_t>(first + i + 1);
            }
        }
    };
};

[[nodiscard]] inline auto BuildChamferedBox() -> ChamferedBoxMesh {
    return ChamferedBoxBuilder::Build();
}

} // namespace ZHLN::Test::GltfFixtures
