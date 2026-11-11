// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TangentGenerator.hpp"
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Physics/Jolt.hpp>
#include <Jolt/Math/Vec3.h>
#include <algorithm>
#include <cmath>

namespace ZHLN::GLTF {

namespace {

[[nodiscard]] auto NormalizeOr(FVec3 v, FVec3 fallback) noexcept -> FVec3 {
    const float len2 = v.LengthSquared();
    return std::isfinite(len2) && len2 > 1e-20f ? v / std::sqrt(len2) : fallback;
}

[[nodiscard]] auto CornerAngle(FVec3 a, FVec3 b) noexcept -> float {
    const float lengths = std::sqrt(a.LengthSquared() * b.LengthSquared());
    return lengths > 1e-20f ? std::acos(std::clamp(Dot(a, b) / lengths, -1.0f, 1.0f)) : 0.0f;
}

} // namespace

// Returns object-space T and the glTF handedness sign for B = cross(N, T) * sign.
// Angle weighting keeps triangulation density from rotating the smooth frame;
// UV seams that reverse handedness require separate vertices.
auto GenerateTangents(
    std::span<const VertexPosition> positions,
    std::span<const std::array<float, 3>> normals,
    std::span<const std::array<float, 2>> texcoords,
    std::span<const uint32_t> indices
) -> std::vector<std::array<float, 4>> {
    const size_t count = positions.size();
    if (normals.size() != count || texcoords.size() != count) return {};

    std::vector<FVec3> tangents(count, FVec3::sZero());
    std::vector<FVec3> bitangents(count, FVec3::sZero());
    const auto position = [&](uint32_t index) -> FVec3 {
        const auto& p = positions[index].position;
        return {p[0], p[1], p[2]};
    };

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
        if (i0 >= count || i1 >= count || i2 >= count) continue;

        const FVec3 p0 = position(i0), p1 = position(i1), p2 = position(i2);
        const FVec3 e1 = p1 - p0, e2 = p2 - p0;
        const auto& uv0 = texcoords[i0];
        const auto& uv1 = texcoords[i1];
        const auto& uv2 = texcoords[i2];
        const float du1 = uv1[0] - uv0[0], dv1 = uv1[1] - uv0[1];
        const float du2 = uv2[0] - uv0[0], dv2 = uv2[1] - uv0[1];
        const float det = du1 * dv2 - dv1 * du2;
        if (std::abs(det) <= 1e-12f || Cross(e1, e2).LengthSquared() <= 1e-20f) continue;

        // Normalize before weighting: a UV island's texel density must not
        // outweigh its neighbours just because its UV gradient is small.
        const FVec3 t = NormalizeOr((e1 * dv2 - e2 * dv1) / det, FVec3::sZero());
        // glTF images have top-left UV origin (V grows down), but their normal
        // maps use OpenGL's +Y-up convention. The shading bitangent is thus
        // -dP/dV. Using +dP/dV makes NormalTangentTest's mapped spheres look
        // concave or rotated; the authored tangents in NormalTangentMirrorTest
        // confirm the opposite handedness for the same UV orientation.
        const FVec3 b = NormalizeOr((e1 * du2 - e2 * du1) / det, FVec3::sZero());
        if (t.LengthSquared() < 0.5f || b.LengthSquared() < 0.5f) continue;

        const float w0 = CornerAngle(e1, e2);
        const float w1 = CornerAngle(p2 - p1, p0 - p1);
        const float w2 = CornerAngle(p0 - p2, p1 - p2);
        tangents[i0] += t * w0;
        tangents[i1] += t * w1;
        tangents[i2] += t * w2;
        bitangents[i0] += b * w0;
        bitangents[i1] += b * w1;
        bitangents[i2] += b * w2;
    }

    std::vector<std::array<float, 4>> result(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& raw = normals[i];
        const FVec3 n = NormalizeOr({raw[0], raw[1], raw[2]}, FVec3::sAxisY());
        const FVec3 projected = tangents[i] - n * Dot(n, tangents[i]);
        const FVec3 axis = std::abs(n.x) < 0.95f ? FVec3::sAxisX() : FVec3::sAxisY();
        const FVec3 fallback = NormalizeOr(axis - n * Dot(n, axis), FVec3::sAxisZ());
        const FVec3 t = NormalizeOr(projected, fallback);
        const float sign = Cross(n, t).Dot(bitangents[i]) < 0.0f ? -1.0f : 1.0f;
        result[i] = {t.x, t.y, t.z, sign};
    }
    return result;
}

} // namespace ZHLN::GLTF
