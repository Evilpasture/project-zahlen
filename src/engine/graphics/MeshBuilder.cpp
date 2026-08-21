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
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace ZHLN::CreativeWorksFactory {

namespace {

/// VK_EXT_mesh_shader: partitions a procedurally generated mesh and uploads the
/// three meshlet streams onto `mesh`. Without this, procedural geometry (boxes,
/// planes, terrain) carries meshletCount == 0 and silently stays on the vertex
/// pipeline forever, even on hardware that supports mesh shading -- only
/// glTF-imported and zcook-cooked meshes would ever take the mesh path.
///
/// `indices` may be empty for the non-indexed builders below: meshlet micro
/// indices address the vertex pool directly, so a trivial 0..n-1 index stream
/// produces exactly the same clusters and leaves the (absent) IBO alone.
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

    mesh.meshletBuffer       = ctx.CreateVertexBuffer(built.meshlets.data(), built.meshlets.size() * sizeof(GPUMeshlet), sizeof(GPUMeshlet));
    mesh.meshletVertexBuffer = ctx.CreateVertexBuffer(built.vertices.data(), built.vertices.size() * sizeof(uint32_t), sizeof(uint32_t));
    mesh.meshletTriBuffer    = ctx.CreateVertexBuffer(built.triangles.data(), built.triangles.size(), sizeof(uint32_t));

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
        if (!res.error().Is(VulkanCallError::FeatureNotPresent)) {
            ZHLN::Log("WARNING: CreateTetrahedronMesh: Failed to build mesh BLAS: {}", res.error().Message());
        }
    }
    return finalMesh;
}

auto CreateBasicMaterial(RenderContext& ctx, bool doubleSided, bool alphaBlend, bool additiveBlend) -> std::expected<Material, Error> {
    PipelineDesc desc;

    // One lookup picks the geometry AND fragment stages together: the scene
    // interface is compiled per pass, so a hand-rolled pairing of, say, the
    // G-buffer vertex shader with PSForward would mismatch varying locations.
    const bool translucent = alphaBlend || additiveBlend;
    const auto shaders     = Resource::GetSceneShaders(translucent ? Resource::SceneShaderVariant::Forward : Resource::SceneShaderVariant::GBuffer);

    desc.vertexShaderData = shaders.vertex.data();
    desc.vertexShaderSize = shaders.vertex.size();
    desc.fragShaderData   = shaders.fragment.data();
    desc.fragShaderSize   = shaders.fragment.size();

    // VK_EXT_mesh_shader: CreateMaterial builds the meshlet pipeline only when
    // the device supports mesh shading; the vertex pipeline above is always
    // built and stays the fallback for skinned meshes and meshes without
    // meshlet streams.
    desc.taskShaderData = shaders.task.data();
    desc.taskShaderSize = shaders.task.size();
    desc.meshShaderData = shaders.mesh.data();
    desc.meshShaderSize = shaders.mesh.size();

    desc.doubleSided   = doubleSided;
    desc.alphaBlend    = alphaBlend;
    desc.additiveBlend = additiveBlend;

    auto mat_res = ctx.CreateMaterial(desc);
    if (!mat_res) {
        return std::unexpected(mat_res.error());
    }
    Material mat  = mat_res.value();
    mat.albedoMap = TextureHandle::Invalid;
    return mat;
}

auto CreateMaterial(RenderContext& ctx, const MaterialDesc& desc) -> std::expected<Material, Error> {
    auto basicMat = CreateBasicMaterial(ctx, desc.doubleSided, desc.alphaBlend, desc.additiveBlend);
    if (!basicMat) {
        return std::unexpected(basicMat.error());
    }

    Material mat        = *basicMat;
    mat.alphaMode       = (desc.alphaMode != 0) ? desc.alphaMode : basicMat->alphaMode;
    mat.alphaCutoff     = desc.alphaCutoff;
    mat.metallicFactor  = desc.metallic;
    mat.roughnessFactor = desc.roughness;
    mat.albedoMap       = desc.albedoMap;
    mat.normalMap       = desc.normalMap;
    mat.pbrMap          = desc.pbrMap;
    mat.emissiveMap     = desc.emissiveMap;

    std::ranges::copy(desc.baseColor, mat.baseColorFactor);
    std::ranges::copy(desc.emissive, mat.emissiveFactor);

    return mat;
}

// ============================================================================
// LOW-LEVEL GPU MESH BUILDERS (RAW GEOMETRY)
// ============================================================================

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
        if (!res.error().Is(VulkanCallError::FeatureNotPresent)) {
            ZHLN::Log("WARNING: CreatePlaneMesh: Failed to build mesh BLAS: {}", res.error().Message());
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
        if (!res.error().Is(VulkanCallError::FeatureNotPresent)) {
            ZHLN::Log("WARNING: CreateBoxMesh: Failed to build mesh BLAS: {}", res.error().Message());
        }
    }
    return finalMesh;
}

auto CreateTerrainMeshFromData(RenderContext& ctx, int sampleCount, float worldSize, const float* heights, const float* colorsRGBA) -> Mesh {
    float halfSize = worldSize / 2.0f;
    float dx       = worldSize / (sampleCount - 1);
    float dz       = worldSize / (sampleCount - 1);

    auto get_height = [&](int x, int z) -> float {
        x = std::clamp(x, 0, sampleCount - 1);
        z = std::clamp(z, 0, sampleCount - 1);
        return heights[x + z * sampleCount];
    };

    auto get_normal = [&](int x, int z) -> JPH::Vec3 {
        float hL = get_height(x - 1, z);
        float hR = get_height(x + 1, z);
        float hD = get_height(x, z - 1);
        float hU = get_height(x, z + 1);
        return JPH::Vec3(hL - hR, 2.0f * dx, hD - hU).Normalized();
    };

    std::vector<VertexPosition>   positions;
    std::vector<VertexAttributes> attributes;
    size_t                        quadCount = static_cast<size_t>(sampleCount - 1) * (sampleCount - 1);
    positions.reserve(quadCount * 6);
    attributes.reserve(quadCount * 6);

    for (int z = 0; z < sampleCount - 1; ++z) {
        for (int x = 0; x < sampleCount - 1; ++x) {
            int idxA = x + z * sampleCount;
            int idxB = (x + 1) + z * sampleCount;
            int idxC = x + (z + 1) * sampleCount;
            int idxD = (x + 1) + (z + 1) * sampleCount;

            float ax  = -halfSize + x * dx;
            float az  = -halfSize + z * dz;
            float bx  = -halfSize + (x + 1) * dx;
            float bz  = -halfSize + z * dz;
            float cx  = -halfSize + x * dx;
            float cz  = -halfSize + (z + 1) * dz;
            float dx_ = -halfSize + (x + 1) * dx;
            float dz_ = -halfSize + (z + 1) * dz;

            JPH::Vec3 nA = get_normal(x, z);
            JPH::Vec3 nB = get_normal(x + 1, z);
            JPH::Vec3 nC = get_normal(x, z + 1);
            JPH::Vec3 nD = get_normal(x + 1, z + 1);

            auto fetch_color = [&](int idx) -> PackedRGBA8 {
                if (colorsRGBA == nullptr) {
                    return Math::PackColor(0.8f, 0.8f, 0.8f, 1.0f);
                }
                const float* c = &colorsRGBA[static_cast<ptrdiff_t>(idx * 4)];
                return Math::PackColor(c[0], c[1], c[2], c[3]);
            };

            VertexPosition   posA {{ax, heights[idxA], az}};
            VertexAttributes attrA {
                .normal  = Math::PackNormal(nA.GetX(), nA.GetY(), nA.GetZ()),
                .tangent = Math::PackNormal(1, 0, 0, 1),
                .uv      = Math::PackUV(static_cast<float>(x) / sampleCount, static_cast<float>(z) / sampleCount),
                .color   = fetch_color(idxA)
            };

            VertexPosition   posB {{bx, heights[idxB], bz}};
            VertexAttributes attrB {
                .normal  = Math::PackNormal(nB.GetX(), nB.GetY(), nB.GetZ()),
                .tangent = Math::PackNormal(1, 0, 0, 1),
                .uv      = Math::PackUV(static_cast<float>(x + 1) / sampleCount, static_cast<float>(z) / sampleCount),
                .color   = fetch_color(idxB)
            };

            VertexPosition   posC {{cx, heights[idxC], cz}};
            VertexAttributes attrC {
                .normal  = Math::PackNormal(nC.GetX(), nC.GetY(), nC.GetZ()),
                .tangent = Math::PackNormal(1, 0, 0, 1),
                .uv      = Math::PackUV(static_cast<float>(x) / sampleCount, static_cast<float>(z + 1) / sampleCount),
                .color   = fetch_color(idxC)
            };

            VertexPosition   posD {{dx_, heights[idxD], dz_}};
            VertexAttributes attrD {
                .normal  = Math::PackNormal(nD.GetX(), nD.GetY(), nD.GetZ()),
                .tangent = Math::PackNormal(1, 0, 0, 1),
                .uv      = Math::PackUV(static_cast<float>(x + 1) / sampleCount, static_cast<float>(z + 1) / sampleCount),
                .color   = fetch_color(idxD)
            };

            positions.push_back(posA);
            attributes.push_back(attrA);
            positions.push_back(posC);
            attributes.push_back(attrC);
            positions.push_back(posB);
            attributes.push_back(attrB);
            positions.push_back(posB);
            attributes.push_back(attrB);
            positions.push_back(posC);
            attributes.push_back(attrC);
            positions.push_back(posD);
            attributes.push_back(attrD);
        }
    }

    BufferHandle posVbo  = ctx.CreateVertexBuffer(positions.data(), positions.size() * sizeof(VertexPosition));
    BufferHandle attrVbo = ctx.CreateVertexBuffer(attributes.data(), attributes.size() * sizeof(VertexAttributes));

    Mesh finalMesh {.posBuffer = posVbo, .attrBuffer = attrVbo, .vertexCount = static_cast<uint32_t>(positions.size())};
    AttachMeshlets(ctx, finalMesh, positions, {});
    if (auto res = ctx.BuildMeshBLAS(finalMesh); !res) [[unlikely]] {
        if (!res.error().Is(VulkanCallError::FeatureNotPresent)) {
            ZHLN::Log("WARNING: CreateTerrainMeshFromData: Failed to build mesh BLAS: {}", res.error().Message());
        }
    }
    return finalMesh;
}

auto CreateTerrainMesh(RenderContext& ctx, int sampleCount, float worldSize, float maxHeight, float* outHeights, TerrainType type) -> Mesh {
    auto hash = [](float x, float y) -> float {
        uint32_t ix = 0;
        std::memcpy(&ix, &x, sizeof(float));
        uint32_t iy = 0;
        std::memcpy(&iy, &y, sizeof(float));
        ix *= 1597u;
        iy *= 5147u;
        uint32_t hashVal = (ix ^ iy) * 0x9E3779B9u;
        return static_cast<float>(hashVal & 0xFFFFFFu) / 16777215.0f;
    };

    auto lerp = [](float a, float b, float t) -> float { return a + t * (b - a); };

    auto noise = [&](float x, float y) -> float {
        float ix = std::floor(x);
        float iy = std::floor(y);
        float fx = x - ix;
        float fy = y - iy;
        float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
        float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
        return lerp(lerp(hash(ix, iy), hash(ix + 1.0f, iy), ux), lerp(hash(ix, iy + 1.0f), hash(ix + 1.0f, iy + 1.0f), ux), uy);
    };

    auto get_height = [&](float x, float z) -> float {
        if (type == TerrainType::Snow) {
            float tx = x * 0.012f;
            float tz = z * 0.012f;

            float warpX = noise(tx + 1.2f, tz + 3.4f);
            float warpZ = noise(tx + 5.6f, tz + 7.8f);

            float val = 0.0f;
            float amp = 0.5f;
            float wx  = tx + warpX * 0.7f;
            float wz  = tz + warpZ * 0.7f;

            for (int i = 0; i < 5; i++) {
                val += amp * noise(wx, wz);
                wx *= 2.05f;
                wz *= 2.05f;
                amp *= 0.48f;
            }

            float ridge = 1.0f - std::abs(noise(tx * 2.2f, tz * 2.2f) * 2.0f - 1.0f);
            ridge *= ridge;

            return (std::pow(val, 1.2f) * 0.75f + ridge * 0.25f) * maxHeight;
        }
        float val  = 0.0f;
        float amp  = 0.5f;
        float freq = 0.015f;
        float tx   = x * freq;
        float tz   = z * freq;
        for (int i = 0; i < 4; i++) {
            val += amp * noise(tx, tz);
            tx *= 2.1f;
            tz *= 2.15f;
            amp *= 0.45f;
        }
        return std::pow(val, 1.4f) * maxHeight;
    };

    float halfSize = worldSize / 2.0f;
    float dx       = worldSize / (sampleCount - 1);
    float dz       = worldSize / (sampleCount - 1);

    for (int z = 0; z < sampleCount; ++z) {
        for (int x = 0; x < sampleCount; ++x) {
            float posX                        = -halfSize + x * dx;
            float posZ                        = -halfSize + z * dz;
            outHeights[x + (z * sampleCount)] = get_height(posX, posZ);
        }
    }

    std::vector<VertexPosition>   positions;
    std::vector<VertexAttributes> attributes;
    positions.reserve(static_cast<size_t>((sampleCount - 1)) * (sampleCount - 1) * 6);
    attributes.reserve(static_cast<size_t>((sampleCount - 1)) * (sampleCount - 1) * 6);

    auto get_normal = [&](int x, int z) -> JPH::Vec3 {
        float     posX = -halfSize + x * dx;
        float     posZ = -halfSize + z * dz;
        float     hL   = (x > 0) ? outHeights[(x - 1) + z * sampleCount] : get_height(posX - dx, posZ);
        float     hR   = (x < sampleCount - 1) ? outHeights[(x + 1) + z * sampleCount] : get_height(posX + dx, posZ);
        float     hD   = (z > 0) ? outHeights[x + (z - 1) * sampleCount] : get_height(posX, posZ - dz);
        float     hU   = (z < sampleCount - 1) ? outHeights[x + (z + 1) * sampleCount] : get_height(posX, posZ + dz);
        JPH::Vec3 normal(hL - hR, 2.0f * dx, hD - hU);
        return normal.Normalized();
    };

    for (int z = 0; z < sampleCount - 1; ++z) {
        for (int x = 0; x < sampleCount - 1; ++x) {
            int idxA = x + z * sampleCount;
            int idxB = (x + 1) + z * sampleCount;
            int idxC = x + (z + 1) * sampleCount;
            int idxD = (x + 1) + (z + 1) * sampleCount;

            float ax  = -halfSize + x * dx;
            float az  = -halfSize + z * dz;
            float bx  = -halfSize + (x + 1) * dx;
            float bz  = -halfSize + z * dz;
            float cx  = -halfSize + x * dx;
            float cz  = -halfSize + (z + 1) * dz;
            float dx_ = -halfSize + (x + 1) * dx;
            float dz_ = -halfSize + (z + 1) * dz;

            float ay = outHeights[idxA];
            float by = outHeights[idxB];
            float cy = outHeights[idxC];
            float dy = outHeights[idxD];

            JPH::Vec3 nA = get_normal(x, z);
            JPH::Vec3 nB = get_normal(x + 1, z);
            JPH::Vec3 nC = get_normal(x, z + 1);
            JPH::Vec3 nD = get_normal(x + 1, z + 1);

            auto get_color = [&](float y, JPH::Vec3 normal) -> PackedRGBA8 {
                float slope = normal.GetY();
                float normY = y / maxHeight;

                if (type == TerrainType::Snow) {
                    if (slope < 0.60f) {
                        return Math::PackColor(0.22f, 0.25f, 0.30f, 1.0f);
                    }
                    if (slope < 0.72f) {
                        float t = (slope - 0.60f) / 0.12f;
                        float r = 0.22f + t * (0.88f - 0.22f);
                        float g = 0.25f + t * (0.93f - 0.25f);
                        float b = 0.30f + t * (0.98f - 0.30f);
                        return Math::PackColor(r, g, b, 1.0f);
                    }
                    if (normY > 0.70f) {
                        return Math::PackColor(0.97f, 0.98f, 1.00f, 1.0f);
                    }
                    if (normY < 0.15f) {
                        return Math::PackColor(0.75f, 0.88f, 0.96f, 1.0f);
                    }
                    float snowVar = 0.90f + 0.06f * std::sin(y * 0.4f);
                    return Math::PackColor(snowVar * 0.95f, snowVar * 0.98f, snowVar, 1.0f);
                }
                if (slope < 0.65f) {
                    return Math::PackColor(0.35f, 0.32f, 0.29f, 1.0f);
                }
                if (normY > 0.75f) {
                    return Math::PackColor(0.95f, 0.95f, 0.98f, 1.0f);
                }
                if (normY < 0.12f) {
                    return Math::PackColor(0.72f, 0.64f, 0.48f, 1.0f);
                }
                float greenVar = 0.4f + 0.12f * std::sin(y * 0.5f);
                return Math::PackColor(0.12f, greenVar, 0.08f, 1.0f);
            };

            VertexPosition   posA  = {{ax, ay, az}};
            VertexAttributes attrA = {
                .normal  = Math::PackNormal(nA.GetX(), nA.GetY(), nA.GetZ()),
                .tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
                .uv      = Math::PackUV(static_cast<float>(x) / sampleCount, static_cast<float>(z) / sampleCount),
                .color   = get_color(ay, nA)
            };

            VertexPosition   vB    = {{bx, by, bz}};
            VertexAttributes attrB = {
                .normal  = Math::PackNormal(nB.GetX(), nB.GetY(), nB.GetZ()),
                .tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
                .uv      = Math::PackUV(static_cast<float>(x + 1) / sampleCount, static_cast<float>(z) / sampleCount),
                .color   = get_color(by, nB)
            };

            VertexPosition   vC    = {{cx, cy, cz}};
            VertexAttributes attrC = {
                .normal  = Math::PackNormal(nC.GetX(), nC.GetY(), nC.GetZ()),
                .tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
                .uv      = Math::PackUV(static_cast<float>(x) / sampleCount, static_cast<float>(z + 1) / sampleCount),
                .color   = get_color(cy, nC)
            };

            VertexPosition   vD    = {{dx_, dy, dz_}};
            VertexAttributes attrD = {
                .normal  = Math::PackNormal(nD.GetX(), nD.GetY(), nD.GetZ()),
                .tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
                .uv      = Math::PackUV(static_cast<float>(x + 1) / sampleCount, static_cast<float>(z + 1) / sampleCount),
                .color   = get_color(dy, nD)
            };

            positions.push_back(posA);
            attributes.push_back(attrA);

            positions.push_back(vC);
            attributes.push_back(attrC);

            positions.push_back(vB);
            attributes.push_back(attrB);

            positions.push_back(vB);
            attributes.push_back(attrB);

            positions.push_back(vC);
            attributes.push_back(attrC);

            positions.push_back(vD);
            attributes.push_back(attrD);
        }
    }

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
        if (!res.error().Is(VulkanCallError::FeatureNotPresent)) {
            ZHLN::Log("WARNING: CreateTerrainMesh: Failed to build mesh BLAS: {}", res.error().Message());
        }
    }
    return finalMesh;
}
} // namespace ZHLN::CreativeWorksFactory
