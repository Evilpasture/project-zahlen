// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TangentGenerator.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <algorithm>
#include <cmath>

namespace ZHLN::GLTF {

namespace {

[[nodiscard]] auto NormalizeOr(JPH::Vec3Arg v, JPH::Vec3Arg fallback) noexcept -> JPH::Vec3 {
    const float len2 = v.LengthSq();
    return std::isfinite(len2) && len2 > 1e-20f ? v / std::sqrt(len2) : fallback;
}

[[nodiscard]] auto CornerAngle(JPH::Vec3Arg a, JPH::Vec3Arg b) noexcept -> float {
    const float lengths = std::sqrt(a.LengthSq() * b.LengthSq());
    return lengths > 1e-20f ? std::acos(std::clamp(a.Dot(b) / lengths, -1.0f, 1.0f)) : 0.0f;
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

    std::vector<JPH::Vec3> tangents(count, JPH::Vec3::sZero());
    std::vector<JPH::Vec3> bitangents(count, JPH::Vec3::sZero());
    const auto position = [&](uint32_t index) -> JPH::Vec3 {
        const auto& p = positions[index].position;
        return {p[0], p[1], p[2]};
    };

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
        if (i0 >= count || i1 >= count || i2 >= count) continue;

        const JPH::Vec3 p0 = position(i0), p1 = position(i1), p2 = position(i2);
        const JPH::Vec3 e1 = p1 - p0, e2 = p2 - p0;
        const auto& uv0 = texcoords[i0];
        const auto& uv1 = texcoords[i1];
        const auto& uv2 = texcoords[i2];
        const float du1 = uv1[0] - uv0[0], dv1 = uv1[1] - uv0[1];
        const float du2 = uv2[0] - uv0[0], dv2 = uv2[1] - uv0[1];
        const float det = du1 * dv2 - dv1 * du2;
        if (std::abs(det) <= 1e-12f || e1.Cross(e2).LengthSq() <= 1e-20f) continue;

        // Normalize before weighting: a UV island's texel density must not
        // outweigh its neighbours just because its UV gradient is small.
        const JPH::Vec3 t = NormalizeOr((e1 * dv2 - e2 * dv1) / det, JPH::Vec3::sZero());
        const JPH::Vec3 b = NormalizeOr((e2 * du1 - e1 * du2) / det, JPH::Vec3::sZero());
        if (t.LengthSq() < 0.5f || b.LengthSq() < 0.5f) continue;

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
        const JPH::Vec3 n = NormalizeOr({raw[0], raw[1], raw[2]}, JPH::Vec3::sAxisY());
        const JPH::Vec3 projected = tangents[i] - n * n.Dot(tangents[i]);
        const JPH::Vec3 axis = std::abs(n.GetX()) < 0.95f ? JPH::Vec3::sAxisX() : JPH::Vec3::sAxisY();
        const JPH::Vec3 fallback = NormalizeOr(axis - n * n.Dot(axis), JPH::Vec3::sAxisZ());
        const JPH::Vec3 t = NormalizeOr(projected, fallback);
        const float sign = n.Cross(t).Dot(bitangents[i]) < 0.0f ? -1.0f : 1.0f;
        result[i] = {t.GetX(), t.GetY(), t.GetZ(), sign};
    }
    return result;
}

} // namespace ZHLN::GLTF
