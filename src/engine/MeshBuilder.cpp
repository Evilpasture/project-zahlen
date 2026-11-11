// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Render/Meshlet.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Render/Render.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace ZHLN::PrefabFactory {

namespace {

struct VertexStreams {
    std::vector<VertexTangentFrame> frames;
    std::vector<VertexSurface>      surfaces;

    void Reserve(size_t count) {
        frames.reserve(count);
        surfaces.reserve(count);
    }

    void Push(Packed1010102 normal, Packed1010102 tangent, PackedHalf2 uv, PackedRGBA8 color) {
        frames.push_back({.normal = normal, .tangent = tangent});
        surfaces.push_back({.uv = uv, .color = color});
    }
};

} // namespace

auto CreateTetrahedronMesh(GraphicsDevice gfx) -> Mesh {
    std::vector<VertexPosition> positions = {{{1.0f, 1.0f, 1.0f}}, {{-1.0f, -1.0f, 1.0f}}, {{-1.0f, 1.0f, -1.0f}}, {{1.0f, -1.0f, -1.0f}}};
    std::vector<uint32_t>       indices   = {0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2};
    VertexStreams               streams;
    Packed1010102               n = Math::PackNormal(0.0f, 1.0f, 0.0f);
    Packed1010102               t = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f);
    PackedRGBA8                 c = Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f);
    streams.Reserve(positions.size());
    for (size_t i = 0; i < positions.size(); ++i) {
        streams.Push(n, t, Math::PackUV(0.0f, 0.0f), c);
    }

    BufferID         posVbo     = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {positions});
    BufferID         frameVbo   = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {streams.frames});
    BufferID         surfaceVbo = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {streams.surfaces});
    BufferID         ibo        = gfx.CreateBuffer<BufferUsage::Index>(std::span {indices});
    const MeshletStreams ml         = gfx.UploadMeshlets(BuildMeshlets(indices, positions));

    Mesh finalMesh = {
        .posBuffer           = posVbo,
        .tangentFrameBuffer  = frameVbo,
        .surfaceBuffer       = surfaceVbo,
        .skinBuffer          = BufferID::Invalid,
        .indexBuffer         = ibo,
        .vertexCount         = static_cast<uint32_t>(positions.size()),
        .indexCount          = static_cast<uint32_t>(indices.size()),
        .meshletBuffer       = ml.meshlets,
        .meshletVertexBuffer = ml.vertices,
        .meshletTriBuffer    = ml.triangles,
        .meshletCount        = ml.count,
    };
    auto res = gfx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::LogWarning("CreateTetrahedronMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

auto CreatePlaneMesh(GraphicsDevice gfx, float extent, ZHLN::FVec4 color) -> Mesh {
    Packed1010102 n = Math::PackNormal(0.0f, 1.0f, 0.0f);
    Packed1010102 t = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f);
    PackedRGBA8   c = Math::PackColor(color.x, color.y, color.z, color.w);

    std::vector<VertexPosition> positions = {{{-extent, 0.0f, extent}}, {{extent, 0.0f, extent}},   {{extent, 0.0f, -extent}},
                                             {{extent, 0.0f, -extent}}, {{-extent, 0.0f, -extent}}, {{-extent, 0.0f, extent}}};

    VertexStreams streams;
    streams.Reserve(positions.size());
    streams.Push(n, t, Math::PackUV(0.0f, 1.0f), c);
    streams.Push(n, t, Math::PackUV(1.0f, 1.0f), c);
    streams.Push(n, t, Math::PackUV(1.0f, 0.0f), c);
    streams.Push(n, t, Math::PackUV(1.0f, 0.0f), c);
    streams.Push(n, t, Math::PackUV(0.0f, 0.0f), c);
    streams.Push(n, t, Math::PackUV(0.0f, 1.0f), c);

    BufferID         posVbo     = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {positions});
    BufferID         frameVbo   = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {streams.frames});
    BufferID         surfaceVbo = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {streams.surfaces});
    const MeshletStreams ml         = gfx.UploadMeshlets(BuildMeshlets(positions));

    auto finalMesh = Mesh {
        .posBuffer           = posVbo,
        .tangentFrameBuffer  = frameVbo,
        .surfaceBuffer       = surfaceVbo,
        .skinBuffer          = BufferID::Invalid,
        .indexBuffer         = BufferID::Invalid,
        .vertexCount         = static_cast<uint32_t>(positions.size()),
        .indexCount          = 0,
        .meshletBuffer       = ml.meshlets,
        .meshletVertexBuffer = ml.vertices,
        .meshletTriBuffer    = ml.triangles,
        .meshletCount        = ml.count,
    };
    auto res = gfx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::LogWarning("CreatePlaneMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

auto CreateBoxMesh(GraphicsDevice gfx, ZHLN::FVec3 halfExtents, ZHLN::FVec4 color) -> Mesh {
    const float x = halfExtents.x;
    const float y = halfExtents.y;
    const float z = halfExtents.z;
    PackedRGBA8 c = Math::PackColor(color.x, color.y, color.z, color.w);

    Packed1010102 nZ  = Math::PackNormal(0, 0, 1);
    Packed1010102 tZ  = Math::PackNormal(1, 0, 0, 1);
    Packed1010102 nNZ = Math::PackNormal(0, 0, -1);
    Packed1010102 tNZ = Math::PackNormal(-1, 0, 0, 1);
    Packed1010102 nY  = Math::PackNormal(0, 1, 0);
    Packed1010102 tY  = Math::PackNormal(1, 0, 0, 1);
    Packed1010102 nNY = Math::PackNormal(0, -1, 0);
    Packed1010102 tNY = Math::PackNormal(1, 0, 0, 1);
    Packed1010102 nX  = Math::PackNormal(1, 0, 0);
    Packed1010102 tX  = Math::PackNormal(0, 0, -1, 1);
    Packed1010102 nNX = Math::PackNormal(-1, 0, 0);
    Packed1010102 tNX = Math::PackNormal(0, 0, 1, 1);

    auto uv00 = Math::PackUV(0.0f, 0.0f);
    auto uv10 = Math::PackUV(1.0f, 0.0f);
    auto uv01 = Math::PackUV(0.0f, 1.0f);
    auto uv11 = Math::PackUV(1.0f, 1.0f);

    std::vector<VertexPosition> positions = {{{-x, -y, z}},  {{x, -y, z}},   {{x, y, z}},   {{x, y, z}},   {{-x, y, z}},  {{-x, -y, z}},
                                             {{x, -y, -z}},  {{-x, -y, -z}}, {{-x, y, -z}}, {{-x, y, -z}}, {{x, y, -z}},  {{x, -y, -z}},
                                             {{-x, y, z}},   {{x, y, z}},    {{x, y, -z}},  {{x, y, -z}},  {{-x, y, -z}}, {{-x, y, z}},
                                             {{-x, -y, -z}}, {{x, -y, -z}},  {{x, -y, z}},  {{x, -y, z}},  {{-x, -y, z}}, {{-x, -y, -z}},
                                             {{x, -y, z}},   {{x, -y, -z}},  {{x, y, -z}},  {{x, y, -z}},  {{x, y, z}},   {{x, -y, z}},
                                             {{-x, -y, -z}}, {{-x, -y, z}},  {{-x, y, z}},  {{-x, y, z}},  {{-x, y, -z}}, {{-x, -y, -z}}};

    VertexStreams streams;
    streams.Reserve(positions.size());
    streams.Push(nZ, tZ, uv01, c);
    streams.Push(nZ, tZ, uv11, c);
    streams.Push(nZ, tZ, uv10, c);
    streams.Push(nZ, tZ, uv10, c);
    streams.Push(nZ, tZ, uv00, c);
    streams.Push(nZ, tZ, uv01, c);
    streams.Push(nNZ, tNZ, uv01, c);
    streams.Push(nNZ, tNZ, uv11, c);
    streams.Push(nNZ, tNZ, uv10, c);
    streams.Push(nNZ, tNZ, uv10, c);
    streams.Push(nNZ, tNZ, uv00, c);
    streams.Push(nNZ, tNZ, uv01, c);
    streams.Push(nY, tY, uv01, c);
    streams.Push(nY, tY, uv11, c);
    streams.Push(nY, tY, uv10, c);
    streams.Push(nY, tY, uv10, c);
    streams.Push(nY, tY, uv00, c);
    streams.Push(nY, tY, uv01, c);
    streams.Push(nNY, tNY, uv01, c);
    streams.Push(nNY, tNY, uv11, c);
    streams.Push(nNY, tNY, uv10, c);
    streams.Push(nNY, tNY, uv10, c);
    streams.Push(nNY, tNY, uv00, c);
    streams.Push(nNY, tNY, uv01, c);
    streams.Push(nX, tX, uv01, c);
    streams.Push(nX, tX, uv11, c);
    streams.Push(nX, tX, uv10, c);
    streams.Push(nX, tX, uv10, c);
    streams.Push(nX, tX, uv00, c);
    streams.Push(nX, tX, uv01, c);
    streams.Push(nNX, tNX, uv01, c);
    streams.Push(nNX, tNX, uv11, c);
    streams.Push(nNX, tNX, uv10, c);
    streams.Push(nNX, tNX, uv10, c);
    streams.Push(nNX, tNX, uv00, c);
    streams.Push(nNX, tNX, uv01, c);

    BufferID         posVbo     = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {positions});
    BufferID         frameVbo   = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {streams.frames});
    BufferID         surfaceVbo = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {streams.surfaces});
    const MeshletStreams ml         = gfx.UploadMeshlets(BuildMeshlets(positions));

    auto finalMesh = Mesh {
        .posBuffer           = posVbo,
        .tangentFrameBuffer  = frameVbo,
        .surfaceBuffer       = surfaceVbo,
        .skinBuffer          = BufferID::Invalid,
        .indexBuffer         = BufferID::Invalid,
        .vertexCount         = static_cast<uint32_t>(positions.size()),
        .indexCount          = 0,
        .meshletBuffer       = ml.meshlets,
        .meshletVertexBuffer = ml.vertices,
        .meshletTriBuffer    = ml.triangles,
        .meshletCount        = ml.count,
    };
    auto res = gfx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::LogWarning("CreateBoxMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

auto CreateSphereMesh(GraphicsDevice gfx, float radius, ZHLN::FVec4 color) -> Mesh {
    constexpr int kRings    = 12;
    constexpr int kSegments = 24;
    const float   r         = radius > 1e-4f ? radius : 1e-4f;
    PackedRGBA8   c         = Math::PackColor(color.x, color.y, color.z, color.w);

    std::vector<VertexPosition> positions;
    VertexStreams               streams;
    positions.reserve(static_cast<size_t>((kRings + 1) * (kSegments + 1)));

    for (int iy = 0; iy <= kRings; ++iy) {
        const float v   = static_cast<float>(iy) / static_cast<float>(kRings);
        const float phi = (v - 0.5f) * JPH::JPH_PI;
        const float y   = JPH::Sin(phi);
        const float rr  = JPH::Cos(phi);
        for (int ix = 0; ix <= kSegments; ++ix) {
            const float     u     = static_cast<float>(ix) / static_cast<float>(kSegments);
            const float     theta = u * 2.0f * JPH::JPH_PI;
            const FVec3 n(rr * JPH::Cos(theta), y, rr * JPH::Sin(theta));
            positions.push_back({n.x * r, n.y * r, n.z * r});
            streams.Push(Math::PackNormal(n.x, n.y, n.z), Math::PackNormal(-JPH::Sin(theta), 0.0f, JPH::Cos(theta)), Math::PackUV(u, v), c);
        }
    }

    std::vector<uint32_t> indices;
    for (int iy = 0; iy < kRings; ++iy) {
        for (int ix = 0; ix < kSegments; ++ix) {
            const uint32_t a  = static_cast<uint32_t>(iy * (kSegments + 1) + ix);
            const uint32_t b  = a + 1;
            const uint32_t cc = a + static_cast<uint32_t>(kSegments + 1);
            const uint32_t d  = cc + 1;
            if (iy != 0) {
                indices.insert(indices.end(), {a, cc, b});
            }
            if (iy != kRings - 1) {
                indices.insert(indices.end(), {b, cc, d});
            }
        }
    }

    BufferID         posVbo     = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {positions});
    BufferID         frameVbo   = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {streams.frames});
    BufferID         surfaceVbo = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {streams.surfaces});
    BufferID         ibo        = gfx.CreateBuffer<BufferUsage::Index>(std::span {indices});
    const MeshletStreams ml         = gfx.UploadMeshlets(BuildMeshlets(indices, positions));

    Mesh finalMesh = {
        .posBuffer           = posVbo,
        .tangentFrameBuffer  = frameVbo,
        .surfaceBuffer       = surfaceVbo,
        .skinBuffer          = BufferID::Invalid,
        .indexBuffer         = ibo,
        .vertexCount         = static_cast<uint32_t>(positions.size()),
        .indexCount          = static_cast<uint32_t>(indices.size()),
        .meshletBuffer       = ml.meshlets,
        .meshletVertexBuffer = ml.vertices,
        .meshletTriBuffer    = ml.triangles,
        .meshletCount        = ml.count,
    };
    auto res = gfx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::LogWarning("CreateSphereMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

auto CreateCylinderMesh(GraphicsDevice gfx, float radius, float height, ZHLN::FVec4 color) -> Mesh {
    constexpr int kSegments = 24;
    const float   r         = radius > 1e-4f ? radius : 1e-4f;
    const float   hy        = (height > 1e-4f ? height : 1e-4f) * 0.5f;
    PackedRGBA8   c         = Math::PackColor(color.x, color.y, color.z, color.w);

    std::vector<VertexPosition> positions;
    VertexStreams               streams;
    std::vector<uint32_t>       indices;

    auto pushVert = [&](const FVec3& pos, const FVec3& n, const FVec3& tangent, float u, float v) -> uint32_t {
        positions.push_back({pos.x, pos.y, pos.z});
        streams.Push(Math::PackNormal(n.x, n.y, n.z), Math::PackNormal(tangent.x, tangent.y, tangent.z), Math::PackUV(u, v), c);
        return static_cast<uint32_t>(positions.size() - 1);
    };

    for (int ix = 0; ix <= kSegments; ++ix) {
        const float     u     = static_cast<float>(ix) / static_cast<float>(kSegments);
        const float     theta = u * 2.0f * JPH::JPH_PI;
        const FVec3 n(JPH::Cos(theta), 0.0f, JPH::Sin(theta));
        const FVec3 t(-JPH::Sin(theta), 0.0f, JPH::Cos(theta));
        pushVert(FVec3(n.x * r, -hy, n.z * r), n, t, u, 0.0f);
        pushVert(FVec3(n.x * r, hy, n.z * r), n, t, u, 1.0f);
    }
    for (int ix = 0; ix < kSegments; ++ix) {
        const uint32_t a = static_cast<uint32_t>(ix * 2);
        const uint32_t b = a + 2;
        indices.insert(indices.end(), {a, a + 1, b, b, a + 1, b + 1});
    }

    for (int sign = 0; sign < 2; ++sign) {
        const float     y     = (sign == 0) ? hy : -hy;
        const FVec3 n     = (sign == 0) ? FVec3(0, 1, 0) : FVec3(0, -1, 0);
        const uint32_t  ct    = pushVert(FVec3(0, y, 0), n, FVec3(1, 0, 0), 0.5f, 0.5f);
        const uint32_t  first = static_cast<uint32_t>(positions.size());
        for (int ix = 0; ix <= kSegments; ++ix) {
            const float theta = static_cast<float>(ix) / static_cast<float>(kSegments) * 2.0f * JPH::JPH_PI;
            pushVert(FVec3(JPH::Cos(theta) * r, y, JPH::Sin(theta) * r), n, FVec3(1, 0, 0), 0.0f, 0.0f);
        }
        for (int ix = 0; ix < kSegments; ++ix) {
            if (sign == 0) {
                indices.insert(indices.end(), {ct, first + static_cast<uint32_t>(ix) + 1, first + static_cast<uint32_t>(ix)});
            } else {
                indices.insert(indices.end(), {ct, first + static_cast<uint32_t>(ix), first + static_cast<uint32_t>(ix) + 1});
            }
        }
    }

    BufferID         posVbo     = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {positions});
    BufferID         frameVbo   = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {streams.frames});
    BufferID         surfaceVbo = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {streams.surfaces});
    BufferID         ibo        = gfx.CreateBuffer<BufferUsage::Index>(std::span {indices});
    const MeshletStreams ml         = gfx.UploadMeshlets(BuildMeshlets(indices, positions));

    Mesh finalMesh = {
        .posBuffer           = posVbo,
        .tangentFrameBuffer  = frameVbo,
        .surfaceBuffer       = surfaceVbo,
        .skinBuffer          = BufferID::Invalid,
        .indexBuffer         = ibo,
        .vertexCount         = static_cast<uint32_t>(positions.size()),
        .indexCount          = static_cast<uint32_t>(indices.size()),
        .meshletBuffer       = ml.meshlets,
        .meshletVertexBuffer = ml.vertices,
        .meshletTriBuffer    = ml.triangles,
        .meshletCount        = ml.count,
    };
    auto res = gfx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::LogWarning("CreateCylinderMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

auto CreateConeMesh(GraphicsDevice gfx, float radius, float height, ZHLN::FVec4 color) -> Mesh {
    constexpr int kSegments = 24;
    const float   r         = radius > 1e-4f ? radius : 1e-4f;
    const float   h         = height > 1e-4f ? height : 1e-4f;
    const float   hy        = h * 0.5f;
    PackedRGBA8   c         = Math::PackColor(color.x, color.y, color.z, color.w);

    const float slant = std::atan2(r, h);
    const float ny    = std::sin(slant);
    const float nr    = std::cos(slant);

    std::vector<VertexPosition> positions;
    VertexStreams               streams;
    std::vector<uint32_t>       indices;

    auto pushVert = [&](const FVec3& pos, const FVec3& n, float u, float v) -> uint32_t {
        positions.push_back({pos.x, pos.y, pos.z});
        streams.Push(Math::PackNormal(n.x, n.y, n.z), Math::PackNormal(1, 0, 0), Math::PackUV(u, v), c);
        return static_cast<uint32_t>(positions.size() - 1);
    };

    const uint32_t apex = pushVert(FVec3(0, hy, 0), FVec3(0, 1, 0), 0.5f, 1.0f);
    for (int ix = 0; ix <= kSegments; ++ix) {
        const float     u     = static_cast<float>(ix) / static_cast<float>(kSegments);
        const float     theta = u * 2.0f * JPH::JPH_PI;
        const FVec3 dir(JPH::Cos(theta), 0.0f, JPH::Sin(theta));
        const FVec3 n(dir.x * nr, ny, dir.z * nr);
        pushVert(FVec3(dir.x * r, -hy, dir.z * r), n, u, 0.0f);
    }
    for (int ix = 0; ix < kSegments; ++ix) {
        indices.insert(indices.end(), {apex, static_cast<uint32_t>(ix + 2), static_cast<uint32_t>(ix + 1)});
    }

    const uint32_t ct    = pushVert(FVec3(0, -hy, 0), FVec3(0, -1, 0), 0.5f, 0.5f);
    const uint32_t first = static_cast<uint32_t>(positions.size());
    for (int ix = 0; ix <= kSegments; ++ix) {
        const float theta = static_cast<float>(ix) / static_cast<float>(kSegments) * 2.0f * JPH::JPH_PI;
        pushVert(FVec3(JPH::Cos(theta) * r, -hy, JPH::Sin(theta) * r), FVec3(0, -1, 0), 0.0f, 0.0f);
    }
    for (int ix = 0; ix < kSegments; ++ix) {
        indices.insert(indices.end(), {ct, first + static_cast<uint32_t>(ix), first + static_cast<uint32_t>(ix) + 1});
    }

    BufferID         posVbo     = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {positions});
    BufferID         frameVbo   = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {streams.frames});
    BufferID         surfaceVbo = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {streams.surfaces});
    BufferID         ibo        = gfx.CreateBuffer<BufferUsage::Index>(std::span {indices});
    const MeshletStreams ml         = gfx.UploadMeshlets(BuildMeshlets(indices, positions));

    Mesh finalMesh = {
        .posBuffer           = posVbo,
        .tangentFrameBuffer  = frameVbo,
        .surfaceBuffer       = surfaceVbo,
        .skinBuffer          = BufferID::Invalid,
        .indexBuffer         = ibo,
        .vertexCount         = static_cast<uint32_t>(positions.size()),
        .indexCount          = static_cast<uint32_t>(indices.size()),
        .meshletBuffer       = ml.meshlets,
        .meshletVertexBuffer = ml.vertices,
        .meshletTriBuffer    = ml.triangles,
        .meshletCount        = ml.count,
    };
    auto res = gfx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::LogWarning("CreateConeMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

} // namespace ZHLN::PrefabFactory
