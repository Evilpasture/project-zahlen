// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Resources.hpp"
#include "Zahlen/Render.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Meshlet.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace ZHLN::CreativeWorksFactory {

namespace {

// VK_EXT_mesh_shader: partitions a procedurally generated mesh and uploads the
// three meshlet streams onto `mesh`. Without this, procedural geometry (boxes,
// planes, terrain) carries meshletCount == 0 and silently stays on the vertex
// pipeline forever, even on hardware that supports mesh shading -- only
// glTF-imported and zcook-cooked meshes would ever take the mesh path.
//
// `indices` may be empty for the non-indexed builders below: meshlet micro
// indices address the vertex pool directly, so a trivial 0..n-1 index stream
// produces exactly the same clusters and leaves the (absent) IBO alone.
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

    // Triangle lists only: anything else has no meshlet representation.
    if (indices.size() < 3 || (indices.size() % 3) != 0) {
        return;
    }

    const auto built = BuildMeshlets(indices, positions);
    if (built.Empty()) {
        return;
    }

    // Storage, not vertex data: the task/mesh shaders reach these only through
    // their device address, they are never bound to the input assembler.
    mesh.meshletBuffer       = ctx.CreateStorageBuffer(built.meshlets.data(), built.meshlets.size() * sizeof(GPUMeshlet), sizeof(GPUMeshlet));
    mesh.meshletVertexBuffer = ctx.CreateStorageBuffer(built.vertices.data(), built.vertices.size() * sizeof(uint32_t), sizeof(uint32_t));
    mesh.meshletTriBuffer    = ctx.CreateStorageBuffer(built.triangles.data(), built.triangles.size(), sizeof(uint8_t));

    if (mesh.meshletBuffer == BufferHandle::Invalid || mesh.meshletVertexBuffer == BufferHandle::Invalid || mesh.meshletTriBuffer == BufferHandle::Invalid) {
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
    std::vector<VertexPosition>   positions = {{{1.0f, 1.0f, 1.0f}}, {{-1.0f, -1.0f, 1.0f}}, {{-1.0f, 1.0f, -1.0f}}, {{1.0f, -1.0f, -1.0f}}};
    std::vector<uint32_t>         indices   = {0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2};
    std::vector<VertexAttributes> attributes;
    Packed1010102                 n = Math::PackNormal(0.0f, 1.0f, 0.0f);
    Packed1010102                 t = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f);
    PackedRGBA8                   c = Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f);
    attributes.reserve(positions.size());
    for (size_t i = 0; i < positions.size(); ++i) {
        attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(0.0f, 0.0f), .color = c});
    }

    BufferHandle posVbo  = ctx.CreateVertexBuffer(positions.data(), positions.size() * sizeof(VertexPosition));
    BufferHandle attrVbo = ctx.CreateVertexBuffer(attributes.data(), attributes.size() * sizeof(VertexAttributes));
    BufferHandle ibo     = ctx.CreateIndexBuffer(indices.data(), indices.size() * sizeof(uint32_t));

    Mesh finalMesh = {
        .posBuffer   = posVbo,
        .attrBuffer  = attrVbo,
        .skinBuffer  = BufferHandle::Invalid,
        .indexBuffer = ibo,
        .vertexCount = static_cast<uint32_t>(positions.size()),
        .indexCount  = static_cast<uint32_t>(indices.size())
    };
    AttachMeshlets(ctx, finalMesh, positions, indices);
    auto res = ctx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::Log("WARNING: CreateTetrahedronMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

// LOW-LEVEL GPU MESH BUILDERS (RAW GEOMETRY)

auto CreatePlaneMesh(RenderContext& ctx, float extent, const JPH::Vec4& color) -> Mesh {
    Packed1010102 n = Math::PackNormal(0.0f, 1.0f, 0.0f);
    Packed1010102 t = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f);
    PackedRGBA8   c = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

    std::vector<VertexPosition> positions = {{{-extent, 0.0f, extent}}, {{extent, 0.0f, extent}},   {{extent, 0.0f, -extent}},
                                             {{extent, 0.0f, -extent}}, {{-extent, 0.0f, -extent}}, {{-extent, 0.0f, extent}}};

    std::vector<VertexAttributes> attributes = {
        {.normal = n, .tangent = t, .uv = Math::PackUV(0.0f, 1.0f), .color = c}, {.normal = n, .tangent = t, .uv = Math::PackUV(1.0f, 1.0f), .color = c},
        {.normal = n, .tangent = t, .uv = Math::PackUV(1.0f, 0.0f), .color = c}, {.normal = n, .tangent = t, .uv = Math::PackUV(1.0f, 0.0f), .color = c},
        {.normal = n, .tangent = t, .uv = Math::PackUV(0.0f, 0.0f), .color = c}, {.normal = n, .tangent = t, .uv = Math::PackUV(0.0f, 1.0f), .color = c}
    };

    BufferHandle posVbo  = ctx.CreateVertexBuffer(positions.data(), positions.size() * sizeof(VertexPosition));
    BufferHandle attrVbo = ctx.CreateVertexBuffer(attributes.data(), attributes.size() * sizeof(VertexAttributes));

    auto finalMesh = Mesh {
        .posBuffer   = posVbo,
        .attrBuffer  = attrVbo,
        .skinBuffer  = BufferHandle::Invalid,
        .indexBuffer = BufferHandle::Invalid,
        .vertexCount = static_cast<uint32_t>(positions.size()),
        .indexCount  = 0
    };
    AttachMeshlets(ctx, finalMesh, positions, {});
    auto res = ctx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::Log("WARNING: CreatePlaneMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

auto CreateBoxMesh(RenderContext& ctx, JPH::Vec3Arg halfExtents, const JPH::Vec4& color) -> Mesh {
    const float x = halfExtents.GetX();
    const float y = halfExtents.GetY();
    const float z = halfExtents.GetZ();
    PackedRGBA8 c = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

    // Front/Back/Top/Bottom/Right/Left normals
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

    std::vector<VertexPosition> positions = {
        // Front (+Z)
        {{-x, -y, z}},
        {{x, -y, z}},
        {{x, y, z}},
        {{x, y, z}},
        {{-x, y, z}},
        {{-x, -y, z}},
        // Back (-Z)
        {{x, -y, -z}},
        {{-x, -y, -z}},
        {{-x, y, -z}},
        {{-x, y, -z}},
        {{x, y, -z}},
        {{x, -y, -z}},
        // Top (+Y)
        {{-x, y, z}},
        {{x, y, z}},
        {{x, y, -z}},
        {{x, y, -z}},
        {{-x, y, -z}},
        {{-x, y, z}},
        // Bottom (-Y)
        {{-x, -y, -z}},
        {{x, -y, -z}},
        {{x, -y, z}},
        {{x, -y, z}},
        {{-x, -y, z}},
        {{-x, -y, -z}},
        // Right (+X)
        {{x, -y, z}},
        {{x, -y, -z}},
        {{x, y, -z}},
        {{x, y, -z}},
        {{x, y, z}},
        {{x, -y, z}},
        // Left (-X)
        {{-x, -y, -z}},
        {{-x, -y, z}},
        {{-x, y, z}},
        {{-x, y, z}},
        {{-x, y, -z}},
        {{-x, -y, -z}}
    };

    std::vector<VertexAttributes> attributes = {
        // Front (+Z)
        {.normal = nZ, .tangent = tZ, .uv = uv01, .color = c},
        {.normal = nZ, .tangent = tZ, .uv = uv11, .color = c},
        {.normal = nZ, .tangent = tZ, .uv = uv10, .color = c},
        {.normal = nZ, .tangent = tZ, .uv = uv10, .color = c},
        {.normal = nZ, .tangent = tZ, .uv = uv00, .color = c},
        {.normal = nZ, .tangent = tZ, .uv = uv01, .color = c},
        // Back (-Z)
        {.normal = nNZ, .tangent = tNZ, .uv = uv01, .color = c},
        {.normal = nNZ, .tangent = tNZ, .uv = uv11, .color = c},
        {.normal = nNZ, .tangent = tNZ, .uv = uv10, .color = c},
        {.normal = nNZ, .tangent = tNZ, .uv = uv10, .color = c},
        {.normal = nNZ, .tangent = tNZ, .uv = uv00, .color = c},
        {.normal = nNZ, .tangent = tNZ, .uv = uv01, .color = c},
        // Top (+Y)
        {.normal = nY, .tangent = tY, .uv = uv01, .color = c},
        {.normal = nY, .tangent = tY, .uv = uv11, .color = c},
        {.normal = nY, .tangent = tY, .uv = uv10, .color = c},
        {.normal = nY, .tangent = tY, .uv = uv10, .color = c},
        {.normal = nY, .tangent = tY, .uv = uv00, .color = c},
        {.normal = nY, .tangent = tY, .uv = uv01, .color = c},
        // Bottom (-Y)
        {.normal = nNY, .tangent = tNY, .uv = uv01, .color = c},
        {.normal = nNY, .tangent = tNY, .uv = uv11, .color = c},
        {.normal = nNY, .tangent = tNY, .uv = uv10, .color = c},
        {.normal = nNY, .tangent = tNY, .uv = uv10, .color = c},
        {.normal = nNY, .tangent = tNY, .uv = uv00, .color = c},
        {.normal = nNY, .tangent = tNY, .uv = uv01, .color = c},
        // Right (+X)
        {.normal = nX, .tangent = tX, .uv = uv01, .color = c},
        {.normal = nX, .tangent = tX, .uv = uv11, .color = c},
        {.normal = nX, .tangent = tX, .uv = uv10, .color = c},
        {.normal = nX, .tangent = tX, .uv = uv10, .color = c},
        {.normal = nX, .tangent = tX, .uv = uv00, .color = c},
        {.normal = nX, .tangent = tX, .uv = uv01, .color = c},
        // Left (-X)
        {.normal = nNX, .tangent = tNX, .uv = uv01, .color = c},
        {.normal = nNX, .tangent = tNX, .uv = uv11, .color = c},
        {.normal = nNX, .tangent = tNX, .uv = uv10, .color = c},
        {.normal = nNX, .tangent = tNX, .uv = uv10, .color = c},
        {.normal = nNX, .tangent = tNX, .uv = uv00, .color = c},
        {.normal = nNX, .tangent = tNX, .uv = uv01, .color = c}
    };

    BufferHandle posVbo  = ctx.CreateVertexBuffer(positions.data(), positions.size() * sizeof(VertexPosition));
    BufferHandle attrVbo = ctx.CreateVertexBuffer(attributes.data(), attributes.size() * sizeof(VertexAttributes));

    auto finalMesh = Mesh {
        .posBuffer   = posVbo,
        .attrBuffer  = attrVbo,
        .skinBuffer  = BufferHandle::Invalid,
        .indexBuffer = BufferHandle::Invalid,
        .vertexCount = static_cast<uint32_t>(positions.size()),
        .indexCount  = 0
    };
    AttachMeshlets(ctx, finalMesh, positions, {});
    auto res = ctx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::Log("WARNING: CreateBoxMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

// UV sphere. The grid carries duplicated seam and pole vertices so normals and
// UVs stay continuous per face; the degenerate pole triangles are simply not
// emitted. Winding matches CreateBoxMesh (counter-clockwise seen from outside).
auto CreateSphereMesh(RenderContext& ctx, float radius, const JPH::Vec4& color) -> Mesh {
    constexpr int kRings    = 12;
    constexpr int kSegments = 24;
    const float   r         = radius > 1e-4f ? radius : 1e-4f;
    PackedRGBA8   c         = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

    std::vector<VertexPosition>   positions;
    std::vector<VertexAttributes> attributes;
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
            attributes.push_back(
                {.normal  = Math::PackNormal(n.GetX(), n.GetY(), n.GetZ()),
                 .tangent = Math::PackNormal(-JPH::Sin(theta), 0.0f, JPH::Cos(theta)),
                 .uv      = Math::PackUV(u, v),
                 .color   = c}
            );
        }
    }

    std::vector<uint32_t> indices;
    for (int iy = 0; iy < kRings; ++iy) {
        for (int ix = 0; ix < kSegments; ++ix) {
            const uint32_t a  = static_cast<uint32_t>(iy * (kSegments + 1) + ix);
            const uint32_t b  = a + 1;
            const uint32_t cc = a + static_cast<uint32_t>(kSegments + 1);
            const uint32_t d  = cc + 1;
            if (iy != 0) { // upper triangle collapses at the top pole
                indices.insert(indices.end(), {a, cc, b});
            }
            if (iy != kRings - 1) { // lower triangle collapses at the bottom pole
                indices.insert(indices.end(), {b, cc, d});
            }
        }
    }

    BufferHandle posVbo  = ctx.CreateVertexBuffer(positions.data(), positions.size() * sizeof(VertexPosition));
    BufferHandle attrVbo = ctx.CreateVertexBuffer(attributes.data(), attributes.size() * sizeof(VertexAttributes));
    BufferHandle ibo     = ctx.CreateIndexBuffer(indices.data(), indices.size() * sizeof(uint32_t));

    Mesh finalMesh = {
        .posBuffer   = posVbo,
        .attrBuffer  = attrVbo,
        .skinBuffer  = BufferHandle::Invalid,
        .indexBuffer = ibo,
        .vertexCount = static_cast<uint32_t>(positions.size()),
        .indexCount  = static_cast<uint32_t>(indices.size())
    };
    AttachMeshlets(ctx, finalMesh, positions, indices);
    auto res = ctx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::Log("WARNING: CreateSphereMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

// Open-ended? No: capped cylinder. Side uses the sphere's winding; the top cap
// fans one way, the bottom the other, so both normals point out of the solid.
auto CreateCylinderMesh(RenderContext& ctx, float radius, float height, const JPH::Vec4& color) -> Mesh {
    constexpr int kSegments = 24;
    const float   r         = radius > 1e-4f ? radius : 1e-4f;
    const float   hy        = (height > 1e-4f ? height : 1e-4f) * 0.5f;
    PackedRGBA8   c         = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

    std::vector<VertexPosition>   positions;
    std::vector<VertexAttributes> attributes;
    std::vector<uint32_t>         indices;

    auto pushVert = [&](const JPH::Vec3& pos, const JPH::Vec3& n, const JPH::Vec3& tangent, float u, float v) -> uint32_t {
        positions.push_back({pos.GetX(), pos.GetY(), pos.GetZ()});
        attributes.push_back(
            {.normal  = Math::PackNormal(n.GetX(), n.GetY(), n.GetZ()),
             .tangent = Math::PackNormal(tangent.GetX(), tangent.GetY(), tangent.GetZ()),
             .uv      = Math::PackUV(u, v),
             .color   = c}
        );
        return static_cast<uint32_t>(positions.size() - 1);
    };

    // Side
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

    // Caps
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

    BufferHandle posVbo  = ctx.CreateVertexBuffer(positions.data(), positions.size() * sizeof(VertexPosition));
    BufferHandle attrVbo = ctx.CreateVertexBuffer(attributes.data(), attributes.size() * sizeof(VertexAttributes));
    BufferHandle ibo     = ctx.CreateIndexBuffer(indices.data(), indices.size() * sizeof(uint32_t));

    Mesh finalMesh = {
        .posBuffer   = posVbo,
        .attrBuffer  = attrVbo,
        .skinBuffer  = BufferHandle::Invalid,
        .indexBuffer = ibo,
        .vertexCount = static_cast<uint32_t>(positions.size()),
        .indexCount  = static_cast<uint32_t>(indices.size())
    };
    AttachMeshlets(ctx, finalMesh, positions, indices);
    auto res = ctx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::Log("WARNING: CreateCylinderMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

// Cone: side fan from the apex (slanted outward normal), flat bottom cap. No
// top cap -- the apex is a point.
auto CreateConeMesh(RenderContext& ctx, float radius, float height, const JPH::Vec4& color) -> Mesh {
    constexpr int kSegments = 24;
    const float   r         = radius > 1e-4f ? radius : 1e-4f;
    const float   h         = height > 1e-4f ? height : 1e-4f;
    const float   hy        = h * 0.5f;
    PackedRGBA8   c         = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

    // Slant: the side normal tilts up by the cone's half-angle.
    const float slant = std::atan2(r, h);
    const float ny    = std::sin(slant);
    const float nr    = std::cos(slant);

    std::vector<VertexPosition>   positions;
    std::vector<VertexAttributes> attributes;
    std::vector<uint32_t>         indices;

    auto pushVert = [&](const JPH::Vec3& pos, const JPH::Vec3& n, float u, float v) -> uint32_t {
        positions.push_back({pos.GetX(), pos.GetY(), pos.GetZ()});
        attributes.push_back(
            {.normal = Math::PackNormal(n.GetX(), n.GetY(), n.GetZ()), .tangent = Math::PackNormal(1, 0, 0), .uv = Math::PackUV(u, v), .color = c}
        );
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

    // Bottom cap
    const uint32_t ct    = pushVert(JPH::Vec3(0, -hy, 0), JPH::Vec3(0, -1, 0), 0.5f, 0.5f);
    const uint32_t first = static_cast<uint32_t>(positions.size());
    for (int ix = 0; ix <= kSegments; ++ix) {
        const float theta = static_cast<float>(ix) / static_cast<float>(kSegments) * 2.0f * JPH::JPH_PI;
        pushVert(JPH::Vec3(JPH::Cos(theta) * r, -hy, JPH::Sin(theta) * r), JPH::Vec3(0, -1, 0), 0.0f, 0.0f);
    }
    for (int ix = 0; ix < kSegments; ++ix) {
        indices.insert(indices.end(), {ct, first + static_cast<uint32_t>(ix), first + static_cast<uint32_t>(ix) + 1});
    }

    BufferHandle posVbo  = ctx.CreateVertexBuffer(positions.data(), positions.size() * sizeof(VertexPosition));
    BufferHandle attrVbo = ctx.CreateVertexBuffer(attributes.data(), attributes.size() * sizeof(VertexAttributes));
    BufferHandle ibo     = ctx.CreateIndexBuffer(indices.data(), indices.size() * sizeof(uint32_t));

    Mesh finalMesh = {
        .posBuffer   = posVbo,
        .attrBuffer  = attrVbo,
        .skinBuffer  = BufferHandle::Invalid,
        .indexBuffer = ibo,
        .vertexCount = static_cast<uint32_t>(positions.size()),
        .indexCount  = static_cast<uint32_t>(indices.size())
    };
    AttachMeshlets(ctx, finalMesh, positions, indices);
    auto res = ctx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::Log("WARNING: CreateConeMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

} // namespace ZHLN::CreativeWorksFactory
