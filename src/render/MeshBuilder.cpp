// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Resources.hpp"
#include "Zahlen/Render/Render.hpp"
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Meshlet.hpp>
#include <Zahlen/PrefabFactory.hpp>
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

void AttachMeshlets(RenderContext& ctx, Mesh& mesh, std::span<const VertexPosition> positions, std::span<const uint32_t> indices) {
    if (positions.empty()) {
        return;
    }

    std::vector<uint32_t> sequential;
    if (indices.empty()) {
        sequential.resize(positions.size());
        for (uint32_t i = 0; i < sequential.size(); ++i) {
            sequential[i] = i;
        }
        indices = sequential;
    }

    if (indices.size() < 3 || (indices.size() % 3) != 0) {
        return;
    }

    const auto built = BuildMeshlets(indices, positions);
    if (built.Empty()) {
        return;
    }

    mesh.meshletBuffer       = ctx.CreateMeshletBuffer(built.meshlets);
    mesh.meshletVertexBuffer = ctx.CreateStorageBuffer(std::span {built.vertices});
    mesh.meshletTriBuffer    = ctx.CreateStorageBuffer(std::span {built.triangles});

    if (mesh.meshletBuffer == BufferHandle::Invalid || mesh.meshletVertexBuffer == BufferHandle::Invalid || mesh.meshletTriBuffer == BufferHandle::Invalid) {
        ctx.DestroyBuffer(mesh.meshletBuffer);
        ctx.DestroyBuffer(mesh.meshletVertexBuffer);
        ctx.DestroyBuffer(mesh.meshletTriBuffer);
        mesh.meshletBuffer       = BufferHandle::Invalid;
        mesh.meshletVertexBuffer = BufferHandle::Invalid;
        mesh.meshletTriBuffer    = BufferHandle::Invalid;
        mesh.meshletCount        = 0;
        return;
    }

    mesh.meshletCount = static_cast<uint32_t>(built.meshlets.size());
}

} // namespace

auto CreateTetrahedronMesh(RenderContext& ctx) -> Mesh {
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

    BufferHandle posVbo     = ctx.CreateVertexBuffer(std::span {positions});
    BufferHandle frameVbo   = ctx.CreateVertexBuffer(std::span {streams.frames});
    BufferHandle surfaceVbo = ctx.CreateVertexBuffer(std::span {streams.surfaces});
    BufferHandle ibo        = ctx.CreateIndexBuffer(std::span {indices});

    Mesh finalMesh = {
        .posBuffer          = posVbo,
        .tangentFrameBuffer = frameVbo,
        .surfaceBuffer      = surfaceVbo,
        .skinBuffer         = BufferHandle::Invalid,
        .indexBuffer        = ibo,
        .vertexCount        = static_cast<uint32_t>(positions.size()),
        .indexCount         = static_cast<uint32_t>(indices.size())
    };
    AttachMeshlets(ctx, finalMesh, positions, indices);
    auto res = ctx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::LogWarning("CreateTetrahedronMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

auto CreatePlaneMesh(RenderContext& ctx, float extent, const JPH::Vec4& color) -> Mesh {
    Packed1010102 n = Math::PackNormal(0.0f, 1.0f, 0.0f);
    Packed1010102 t = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f);
    PackedRGBA8   c = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

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

    BufferHandle posVbo     = ctx.CreateVertexBuffer(std::span {positions});
    BufferHandle frameVbo   = ctx.CreateVertexBuffer(std::span {streams.frames});
    BufferHandle surfaceVbo = ctx.CreateVertexBuffer(std::span {streams.surfaces});

    auto finalMesh = Mesh {
        .posBuffer          = posVbo,
        .tangentFrameBuffer = frameVbo,
        .surfaceBuffer      = surfaceVbo,
        .skinBuffer         = BufferHandle::Invalid,
        .indexBuffer        = BufferHandle::Invalid,
        .vertexCount        = static_cast<uint32_t>(positions.size()),
        .indexCount         = 0
    };
    AttachMeshlets(ctx, finalMesh, positions, {});
    auto res = ctx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::LogWarning("CreatePlaneMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

auto CreateBoxMesh(RenderContext& ctx, JPH::Vec3Arg halfExtents, const JPH::Vec4& color) -> Mesh {
    const float x = halfExtents.GetX();
    const float y = halfExtents.GetY();
    const float z = halfExtents.GetZ();
    PackedRGBA8 c = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

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

    BufferHandle posVbo     = ctx.CreateVertexBuffer(std::span {positions});
    BufferHandle frameVbo   = ctx.CreateVertexBuffer(std::span {streams.frames});
    BufferHandle surfaceVbo = ctx.CreateVertexBuffer(std::span {streams.surfaces});

    auto finalMesh = Mesh {
        .posBuffer          = posVbo,
        .tangentFrameBuffer = frameVbo,
        .surfaceBuffer      = surfaceVbo,
        .skinBuffer         = BufferHandle::Invalid,
        .indexBuffer        = BufferHandle::Invalid,
        .vertexCount        = static_cast<uint32_t>(positions.size()),
        .indexCount         = 0
    };
    AttachMeshlets(ctx, finalMesh, positions, {});
    auto res = ctx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::LogWarning("CreateBoxMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

auto CreateSphereMesh(RenderContext& ctx, float radius, const JPH::Vec4& color) -> Mesh {
    constexpr int kRings    = 12;
    constexpr int kSegments = 24;
    const float   r         = radius > 1e-4f ? radius : 1e-4f;
    PackedRGBA8   c         = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

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
            const JPH::Vec3 n(rr * JPH::Cos(theta), y, rr * JPH::Sin(theta));
            positions.push_back({n.GetX() * r, n.GetY() * r, n.GetZ() * r});
            streams.Push(Math::PackNormal(n.GetX(), n.GetY(), n.GetZ()), Math::PackNormal(-JPH::Sin(theta), 0.0f, JPH::Cos(theta)), Math::PackUV(u, v), c);
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

    BufferHandle posVbo     = ctx.CreateVertexBuffer(std::span {positions});
    BufferHandle frameVbo   = ctx.CreateVertexBuffer(std::span {streams.frames});
    BufferHandle surfaceVbo = ctx.CreateVertexBuffer(std::span {streams.surfaces});
    BufferHandle ibo        = ctx.CreateIndexBuffer(std::span {indices});

    Mesh finalMesh = {
        .posBuffer          = posVbo,
        .tangentFrameBuffer = frameVbo,
        .surfaceBuffer      = surfaceVbo,
        .skinBuffer         = BufferHandle::Invalid,
        .indexBuffer        = ibo,
        .vertexCount        = static_cast<uint32_t>(positions.size()),
        .indexCount         = static_cast<uint32_t>(indices.size())
    };
    AttachMeshlets(ctx, finalMesh, positions, indices);
    auto res = ctx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::LogWarning("CreateSphereMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

auto CreateCylinderMesh(RenderContext& ctx, float radius, float height, const JPH::Vec4& color) -> Mesh {
    constexpr int kSegments = 24;
    const float   r         = radius > 1e-4f ? radius : 1e-4f;
    const float   hy        = (height > 1e-4f ? height : 1e-4f) * 0.5f;
    PackedRGBA8   c         = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

    std::vector<VertexPosition> positions;
    VertexStreams               streams;
    std::vector<uint32_t>       indices;

    auto pushVert = [&](const JPH::Vec3& pos, const JPH::Vec3& n, const JPH::Vec3& tangent, float u, float v) -> uint32_t {
        positions.push_back({pos.GetX(), pos.GetY(), pos.GetZ()});
        streams.Push(Math::PackNormal(n.GetX(), n.GetY(), n.GetZ()), Math::PackNormal(tangent.GetX(), tangent.GetY(), tangent.GetZ()), Math::PackUV(u, v), c);
        return static_cast<uint32_t>(positions.size() - 1);
    };

    for (int ix = 0; ix <= kSegments; ++ix) {
        const float     u     = static_cast<float>(ix) / static_cast<float>(kSegments);
        const float     theta = u * 2.0f * JPH::JPH_PI;
        const JPH::Vec3 n(JPH::Cos(theta), 0.0f, JPH::Sin(theta));
        const JPH::Vec3 t(-JPH::Sin(theta), 0.0f, JPH::Cos(theta));
        pushVert(JPH::Vec3(n.GetX() * r, -hy, n.GetZ() * r), n, t, u, 0.0f);
        pushVert(JPH::Vec3(n.GetX() * r, hy, n.GetZ() * r), n, t, u, 1.0f);
    }
    for (int ix = 0; ix < kSegments; ++ix) {
        const uint32_t a = static_cast<uint32_t>(ix * 2);
        const uint32_t b = a + 2;
        indices.insert(indices.end(), {a, a + 1, b, b, a + 1, b + 1});
    }

    for (int sign = 0; sign < 2; ++sign) {
        const float     y     = (sign == 0) ? hy : -hy;
        const JPH::Vec3 n     = (sign == 0) ? JPH::Vec3(0, 1, 0) : JPH::Vec3(0, -1, 0);
        const uint32_t  ct    = pushVert(JPH::Vec3(0, y, 0), n, JPH::Vec3(1, 0, 0), 0.5f, 0.5f);
        const uint32_t  first = static_cast<uint32_t>(positions.size());
        for (int ix = 0; ix <= kSegments; ++ix) {
            const float theta = static_cast<float>(ix) / static_cast<float>(kSegments) * 2.0f * JPH::JPH_PI;
            pushVert(JPH::Vec3(JPH::Cos(theta) * r, y, JPH::Sin(theta) * r), n, JPH::Vec3(1, 0, 0), 0.0f, 0.0f);
        }
        for (int ix = 0; ix < kSegments; ++ix) {
            if (sign == 0) {
                indices.insert(indices.end(), {ct, first + static_cast<uint32_t>(ix) + 1, first + static_cast<uint32_t>(ix)});
            } else {
                indices.insert(indices.end(), {ct, first + static_cast<uint32_t>(ix), first + static_cast<uint32_t>(ix) + 1});
            }
        }
    }

    BufferHandle posVbo     = ctx.CreateVertexBuffer(std::span {positions});
    BufferHandle frameVbo   = ctx.CreateVertexBuffer(std::span {streams.frames});
    BufferHandle surfaceVbo = ctx.CreateVertexBuffer(std::span {streams.surfaces});
    BufferHandle ibo        = ctx.CreateIndexBuffer(std::span {indices});

    Mesh finalMesh = {
        .posBuffer          = posVbo,
        .tangentFrameBuffer = frameVbo,
        .surfaceBuffer      = surfaceVbo,
        .skinBuffer         = BufferHandle::Invalid,
        .indexBuffer        = ibo,
        .vertexCount        = static_cast<uint32_t>(positions.size()),
        .indexCount         = static_cast<uint32_t>(indices.size())
    };
    AttachMeshlets(ctx, finalMesh, positions, indices);
    auto res = ctx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::LogWarning("CreateCylinderMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

auto CreateConeMesh(RenderContext& ctx, float radius, float height, const JPH::Vec4& color) -> Mesh {
    constexpr int kSegments = 24;
    const float   r         = radius > 1e-4f ? radius : 1e-4f;
    const float   h         = height > 1e-4f ? height : 1e-4f;
    const float   hy        = h * 0.5f;
    PackedRGBA8   c         = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

    const float slant = std::atan2(r, h);
    const float ny    = std::sin(slant);
    const float nr    = std::cos(slant);

    std::vector<VertexPosition> positions;
    VertexStreams               streams;
    std::vector<uint32_t>       indices;

    auto pushVert = [&](const JPH::Vec3& pos, const JPH::Vec3& n, float u, float v) -> uint32_t {
        positions.push_back({pos.GetX(), pos.GetY(), pos.GetZ()});
        streams.Push(Math::PackNormal(n.GetX(), n.GetY(), n.GetZ()), Math::PackNormal(1, 0, 0), Math::PackUV(u, v), c);
        return static_cast<uint32_t>(positions.size() - 1);
    };

    const uint32_t apex = pushVert(JPH::Vec3(0, hy, 0), JPH::Vec3(0, 1, 0), 0.5f, 1.0f);
    for (int ix = 0; ix <= kSegments; ++ix) {
        const float     u     = static_cast<float>(ix) / static_cast<float>(kSegments);
        const float     theta = u * 2.0f * JPH::JPH_PI;
        const JPH::Vec3 dir(JPH::Cos(theta), 0.0f, JPH::Sin(theta));
        const JPH::Vec3 n(dir.GetX() * nr, ny, dir.GetZ() * nr);
        pushVert(JPH::Vec3(dir.GetX() * r, -hy, dir.GetZ() * r), n, u, 0.0f);
    }
    for (int ix = 0; ix < kSegments; ++ix) {
        indices.insert(indices.end(), {apex, static_cast<uint32_t>(ix + 2), static_cast<uint32_t>(ix + 1)});
    }

    const uint32_t ct    = pushVert(JPH::Vec3(0, -hy, 0), JPH::Vec3(0, -1, 0), 0.5f, 0.5f);
    const uint32_t first = static_cast<uint32_t>(positions.size());
    for (int ix = 0; ix <= kSegments; ++ix) {
        const float theta = static_cast<float>(ix) / static_cast<float>(kSegments) * 2.0f * JPH::JPH_PI;
        pushVert(JPH::Vec3(JPH::Cos(theta) * r, -hy, JPH::Sin(theta) * r), JPH::Vec3(0, -1, 0), 0.0f, 0.0f);
    }
    for (int ix = 0; ix < kSegments; ++ix) {
        indices.insert(indices.end(), {ct, first + static_cast<uint32_t>(ix), first + static_cast<uint32_t>(ix) + 1});
    }

    BufferHandle posVbo     = ctx.CreateVertexBuffer(std::span {positions});
    BufferHandle frameVbo   = ctx.CreateVertexBuffer(std::span {streams.frames});
    BufferHandle surfaceVbo = ctx.CreateVertexBuffer(std::span {streams.surfaces});
    BufferHandle ibo        = ctx.CreateIndexBuffer(std::span {indices});

    Mesh finalMesh = {
        .posBuffer          = posVbo,
        .tangentFrameBuffer = frameVbo,
        .surfaceBuffer      = surfaceVbo,
        .skinBuffer         = BufferHandle::Invalid,
        .indexBuffer        = ibo,
        .vertexCount        = static_cast<uint32_t>(positions.size()),
        .indexCount         = static_cast<uint32_t>(indices.size())
    };
    AttachMeshlets(ctx, finalMesh, positions, indices);
    auto res = ctx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::LogWarning("CreateConeMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

} // namespace ZHLN::PrefabFactory
