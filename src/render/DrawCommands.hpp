// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/DrawCommands.hpp
//
// What a frame's draw submission is made of: the GPU-resident mesh and material
// records the queues point at, the per-queue command payloads, and the queues
// themselves. Pure data -- no pass, no pipeline, no device state.
//
// These live in their own header rather than in RenderInternal.hpp for one
// structural reason: RenderContext::Impl's destructor is defined inline there,
// so a manager holding an incomplete type cannot be a member of Impl. Any
// manager that owns a queue therefore needs these types visible without
// dragging all 1100 lines of Impl in behind them.
//
// Two include rules keep this header compile-checkable on its own:
//
//   * <Zahlen/Render/GpuLayout.hpp> is included because DrawCommand holds
//     InstanceData by value and the emitter commands hold their parameter
//     structs by value. That is a real dependency, not an oversight -- the
//     generated structs are part of the payload's layout.
//   * src/render/GpuAbi.hpp is deliberately NOT included. It is the header that
//     makes a render translation unit uncompilable without the shader cook
//     (#embed ZHLN_GPU_ABI_MODULE plus a consteval parse of the cooked module),
//     and nothing here needs the ABI check -- only the structs.

#pragma once
#include "Rendering.hpp"

#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Render/GpuLayout.hpp> // InstanceData, ParticleEmitterParams, MeshParticleEmitterParams
#include <Zahlen/Render/Types.hpp>     // DrawFlags, CSGOperation, BufferHandle, JPH math
#include <array>
#include <cstdint>
#include <span>
#include <type_traits>

namespace ZHLN {

// ---------------------------------------------------------------------------
// GPU-resident records the queues point at
// ---------------------------------------------------------------------------

struct NativeMesh {
    Vk::Buffer                 buffer;
    uint32_t                   vertexCount = 0;
    VkDeviceAddress            vboAddress  = 0;
    // Self-owning BLAS: the handle carries the device it was created on, so
    // the destructor retires it through DeviceHandle with no other reference.
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
    VkPipelineLayout layout = VK_NULL_HANDLE; // Non-owning alias of the spec-required null heap layout

    // VK_EXT_mesh_shader variant of the same material (task+mesh+fragment); invalid
    // when the device cannot mesh-shade or the material opted out, and draw submission
    // then falls back to `pipeline`.
    Vk::Pipeline meshPipeline;

    [[nodiscard]] bool HasMeshPipeline() const noexcept {
        return meshPipeline.Valid();
    }
};

// ---------------------------------------------------------------------------
// Per-queue command payloads
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// The frame's queues
// ---------------------------------------------------------------------------

// A plain aggregate. The frame lifecycle around these -- clearing, sorting,
// per-queue access -- lives in DrawQueueManager, which is the only thing that
// touches this struct.
struct RenderQueues {
    ZHLN::Array<DrawCommand>                drawQueue;
    ZHLN::Array<CSGDrawCommand>             csgDrawQueue;
    ZHLN::Array<ParticleEmitterCommand>     particleEmittersQueue;
    ZHLN::Array<MeshParticleEmitterCommand> meshParticleQueue;
    ZHLN::Array<DecalDrawCommand>           decalQueue;
    ZHLN::Array<LineSegment>                lineQueue;
};

} // namespace ZHLN
