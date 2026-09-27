// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include "Rendering.hpp"

#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Render/GpuLayout.hpp>
#include <Zahlen/Render/Types.hpp>
#include <array>
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
    NativeMesh(
        Vk::Buffer&&              buf,
        uint32_t                  count,
        VkDeviceAddress           vboAddr,
        Vk::AccelerationStructure b    = {},
        VkDeviceAddress           addr = 0,
        Vk::Buffer&&              bBuf = {}
    ): buffer(std::move(buf)), vertexCount(count), vboAddress(vboAddr), blas(std::move(b)), blasAddress(addr), blasBuffer(std::move(bBuf)) {
    }
};

struct NativeMaterial {
    Vk::Pipeline     pipeline;
    VkPipelineLayout layout = VK_NULL_HANDLE;

    Vk::Pipeline meshPipeline;

    [[nodiscard]] bool HasMeshPipeline() const noexcept {
        return meshPipeline.Valid();
    }
};


struct DrawCommand {
    InstanceData         instanceData;
    NativeMaterial*      material;
    NativeMaterial*      prePassMaterial;
    NativeMesh*          posMesh;
    NativeMesh*          attrMesh;
    NativeMesh*          skinMesh;
    BufferHandle         skinnedVertexBuffer;
    uint32_t             jointOffset;
    uint32_t             morphOffset;
    uint32_t             activeMorphCount;
    std::array<float, 4> morphWeights;
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
