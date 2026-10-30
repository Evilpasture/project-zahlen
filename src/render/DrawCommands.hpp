// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Rendering.hpp"

#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include "GpuLayout.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Render/Vertex.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace ZHLN {

struct NativeMesh {
    Vk::Buffer                 buffer;
    uint32_t                   vertexCount = 0;

    Vk::DeviceAddress<>        vboAddress;
    Vk::AccelerationStructure  blas;
    Vk::DeviceAddress<>        blasAddress;
    Vk::Buffer                 blasBuffer;

    NativeMesh() = default;

    NativeMesh(
        Vk::Buffer                buf,
        uint32_t                  count,
        Vk::DeviceAddress<>       vboAddr,
        Vk::AccelerationStructure b    = {},
        Vk::DeviceAddress<>       addr = nullptr,
        Vk::Buffer                bBuf = {}
    ): buffer(std::move(buf)), vertexCount(count), vboAddress(vboAddr), blas(std::move(b)), blasAddress(addr), blasBuffer(std::move(bBuf)) {
    }
};

struct NativeMaterial {
    VkPipeline pipeline     = VK_NULL_HANDLE;
    VkPipeline meshPipeline = VK_NULL_HANDLE;

    NativeMaterial() = default;
    NativeMaterial(const NativeMaterial&) = delete;
    auto operator=(const NativeMaterial&) -> NativeMaterial& = delete;
    NativeMaterial(NativeMaterial&&) = delete;
    auto operator=(NativeMaterial&&) -> NativeMaterial& = delete;

    [[nodiscard]] bool HasMeshPipeline() const noexcept {
        return meshPipeline != VK_NULL_HANDLE;
    }
};
static_assert(std::is_trivially_destructible_v<NativeMaterial> && !std::is_copy_constructible_v<NativeMaterial>);

[[nodiscard]] auto PackMaterialSamplerAddresses(const MaterialSamplerAddresses& addresses, size_t first) noexcept -> uint32_t;

struct DrawCommand {
    InstanceData         instanceData;
    NativeMaterial*      material;
    NativeMaterial*      prePassMaterial;

    Vk::DeviceAddress<>                    blasAddress;
    Vk::DeviceAddress<VertexPosition>      skinPosAddress;
    Vk::DeviceAddress<VertexTangentFrame>  skinFrameAddress;

    Vk::DeviceAddress<uint32_t>            skinDataAddress;
    BufferHandle                           skinnedVertexBuffer;
    uint32_t             jointOffset;
    uint32_t             morphOffset;
    uint32_t             activeMorphCount;
    JPH::Float4                            morphWeights;

    DrawFlags            flags;
};

static_assert(std::is_trivially_copyable_v<DrawCommand> && std::is_trivially_constructible_v<DrawCommand>);

struct SkinnedBlasEntry {
    BufferHandle        scratch;
    Vk::DeviceAddress<> address;
};

[[nodiscard]] auto RebuildSkinnedBLAS(
    VkCommandBuffer cmd, Vk::Context& ctx, Vk::Allocator& allocator, Vk::DeletionQueue& deletionQueue, const DrawCommand& drawCmd,
    NativeMesh& scratchMesh
) -> std::expected<Vk::DeviceAddress<>, ErrorCode>;

struct CSGDrawCommand {
    DrawCommand eyeDraw;
    uint32_t    eyeInstanceIdx;

    struct Cutter {
        DrawCommand  draw;
        uint32_t     instanceIdx;
        CSGOperation operation;
    };
    ZHLN::Array<Cutter> cutters;
};

struct ParticleEmitterCommand {
    BufferHandle          gpuBuffer;
    uint32_t              maxParticles;
    ParticleEmitterParams params;

    bool simulate = true;
};

static_assert(std::is_trivially_copyable_v<ParticleEmitterCommand> && std::is_standard_layout_v<ParticleEmitterCommand>);

struct DecalDrawCommand {
    JPH::Mat44 transform;
    JPH::Mat44 invTransform;
    uint32_t   albedoIndex;
    uint32_t   normalIndex;
    float      roughness;
    float      metallic;
};

struct MeshParticleEmitterCommand {
    BufferHandle              gpuBuffer;
    uint32_t                  maxParticles;
    MeshParticleEmitterParams params;
    AssetID                   meshAsset;
    MaterialID                materialAsset;
};

struct RenderQueues {
    ZHLN::Array<DrawCommand>                drawQueue;
    ZHLN::Array<CSGDrawCommand>             csgDrawQueue;
    ZHLN::Array<ParticleEmitterCommand>     particleEmittersQueue;
    ZHLN::Array<MeshParticleEmitterCommand> meshParticleQueue;
    ZHLN::Array<DecalDrawCommand>           decalQueue;
};

}
