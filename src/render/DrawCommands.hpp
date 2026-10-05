// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include "Rendering.hpp"

#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include "GpuLayout.hpp"
#include <Zahlen/Render/Types.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace ZHLN {


struct NativeMesh {
    Vk::Buffer                 buffer;
    uint32_t                   vertexCount = 0;
    VkDeviceAddress            vboAddress  = 0;
    Vk::AccelerationStructure  blas;
    VkDeviceAddress            blasAddress = 0;
    Vk::Buffer                 blasBuffer;

    NativeMesh() = default;
    // Both buffers are taken by value: they are move-only handles, so this is the same
    // single move with the ownership stated in the signature, as TextureManager::Adopt
    // does with its image and view.
    NativeMesh(
        Vk::Buffer                buf,
        uint32_t                  count,
        VkDeviceAddress           vboAddr,
        Vk::AccelerationStructure b    = {},
        VkDeviceAddress           addr = 0,
        Vk::Buffer                bBuf = {}
    ): buffer(std::move(buf)), vertexCount(count), vboAddress(vboAddr), blas(std::move(b)), blasAddress(addr), blasBuffer(std::move(bBuf)) {
    }
};

// Stable registry entry: GPU pipelines are retired by PipelineRegistry, never
// by destruction of the pool slot. The layout is borrowed from the renderer.
struct NativeMaterial {
    VkPipeline       pipeline     = VK_NULL_HANDLE;
    VkPipelineLayout layout       = VK_NULL_HANDLE;
    VkPipeline       meshPipeline = VK_NULL_HANDLE;

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


// Draw-submission translation: material S/T modes -> common.slang sampler words.
// Kept private so shader packing changes do not become public Material API changes.
[[nodiscard]] auto PackMaterialSamplerAddresses(const MaterialSamplerAddresses& addresses, size_t first) noexcept -> uint32_t;

struct DrawCommand {
    InstanceData         instanceData;
    NativeMaterial*      material;
    NativeMaterial*      prePassMaterial;
    NativeMesh*          posMesh;
    NativeMesh*          frameMesh;
    NativeMesh*          skinMesh;
    BufferHandle         skinnedVertexBuffer;
    uint32_t             jointOffset;
    uint32_t             morphOffset;
    uint32_t             activeMorphCount;
    JPH::Float4          morphWeights; // no initializer: a default member initializer
                                       // would make DrawCommand non-trivially
                                       // constructible, which the assert below
                                       // forbids. Every construction path fills it.
    DrawFlags            flags;
};

static_assert(std::is_trivially_copyable_v<DrawCommand> && std::is_trivially_constructible_v<DrawCommand>);

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
    // False for quads the host already integrated on the CPU (DrawBillboards): the
    // update pass must render them, not simulate them a second time.
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

struct LineSegment {
    JPH::Vec3 start      = JPH::Vec3::sZero();
    JPH::Vec3 end        = JPH::Vec3::sZero();
    JPH::Vec4 colorStart = {1.0f, 1.0f, 1.0f, 1.0f};
    JPH::Vec4 colorEnd   = {1.0f, 1.0f, 1.0f, 1.0f};
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
    ZHLN::Array<LineSegment>                lineQueue;
};

}
