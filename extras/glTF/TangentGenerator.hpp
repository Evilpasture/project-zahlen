// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// glTF permits NORMAL + TEXCOORD_0/1 without TANGENT. A normal map still needs
// a tangent frame derived from the mesh's own UV gradients; a fixed +X tangent
// makes the map rotate (or invert) on differently oriented UV islands.
// Authored TANGENT attributes are never passed through this generator.

#include <Zahlen/Vertex.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ZHLN::GLTF {

namespace TangentDetail {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    auto operator+=(Vec3 other) noexcept -> Vec3& {
        x += other.x;
        y += other.y;
        z += other.z;
        return *this;
    }
};

[[nodiscard]] constexpr auto operator-(Vec3 a, Vec3 b) noexcept -> Vec3 { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] constexpr auto operator*(Vec3 a, float scale) noexcept -> Vec3 { return {a.x * scale, a.y * scale, a.z * scale}; }
[[nodiscard]] constexpr auto Dot(Vec3 a, Vec3 b) noexcept -> float { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] constexpr auto Cross(Vec3 a, Vec3 b) noexcept -> Vec3 {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline auto NormalizeOr(Vec3 v, Vec3 fallback) noexcept -> Vec3 {
    const float len2 = Dot(v, v);
    return std::isfinite(len2) && len2 > 1e-20f ? v * (1.0f / std::sqrt(len2)) : fallback;
}
[[nodiscard]] inline auto CornerAngle(Vec3 a, Vec3 b) noexcept -> float {
    const float lengths = std::sqrt(Dot(a, a) * Dot(b, b));
    return lengths > 1e-20f ? std::acos(std::clamp(Dot(a, b) / lengths, -1.0f, 1.0f)) : 0.0f;
}

} // namespace TangentDetail

// Returns normalized object-space T and the glTF handedness sign for
// B = cross(N, T) * sign. Angle weighting keeps triangulation density from
// rotating the smooth basis; UV seams that reverse handedness require distinct
// vertices (one four-component tangent cannot represent both sides).
[[nodiscard]] inline auto GenerateTangents(
    std::span<const VertexPosition> positions,
    std::span<const std::array<float, 3>> normals,
    std::span<const std::array<float, 2>> texcoords,
    std::span<const uint32_t> indices
) -> std::vector<std::array<float, 4>> {
    using namespace TangentDetail;
    const size_t count = positions.size();
    if (normals.size() != count || texcoords.size() != count) return {};

    std::vector<Vec3> tangents(count);
    std::vector<Vec3> bitangents(count);
    const auto position = [&](uint32_t index) -> Vec3 {
        const auto& p = positions[index].position;
        return {p[0], p[1], p[2]};
    };

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
        if (i0 >= count || i1 >= count || i2 >= count) continue;

        const Vec3 p0 = position(i0), p1 = position(i1), p2 = position(i2);
        const Vec3 e1 = p1 - p0, e2 = p2 - p0;
        const auto& uv0 = texcoords[i0];
        const auto& uv1 = texcoords[i1];
        const auto& uv2 = texcoords[i2];
        const float du1 = uv1[0] - uv0[0], dv1 = uv1[1] - uv0[1];
        const float du2 = uv2[0] - uv0[0], dv2 = uv2[1] - uv0[1];
        const float det = du1 * dv2 - dv1 * du2;
        const Vec3 faceNormal = Cross(e1, e2);
        if (std::abs(det) <= 1e-12f || Dot(faceNormal, faceNormal) <= 1e-20f) continue;

        // Normalize before weighting: a UV island's texel density must not
        // outweigh its neighbours just because its UV gradient is small.
        const Vec3 t = NormalizeOr((e1 * dv2 - e2 * dv1) * (1.0f / det), {});
        const Vec3 b = NormalizeOr((e2 * du1 - e1 * du2) * (1.0f / det), {});
        if (Dot(t, t) < 0.5f || Dot(b, b) < 0.5f) continue;

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
        const Vec3 n = NormalizeOr({raw[0], raw[1], raw[2]}, {0.0f, 1.0f, 0.0f});
        const Vec3 projected = tangents[i] - n * Dot(n, tangents[i]);
        const Vec3 axis = std::abs(n.x) < 0.95f ? Vec3 {1.0f, 0.0f, 0.0f} : Vec3 {0.0f, 1.0f, 0.0f};
        const Vec3 fallback = NormalizeOr(axis - n * Dot(n, axis), {0.0f, 0.0f, 1.0f});
        const Vec3 t = NormalizeOr(projected, fallback);
        const float sign = Dot(Cross(n, t), bitangents[i]) < 0.0f ? -1.0f : 1.0f;
        result[i] = {t.x, t.y, t.z, sign};
    }
    return result;
}

} // namespace ZHLN::GLTF
