// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include "Rendering.hpp"

#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include "GpuLayout.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Vertex.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace ZHLN {


struct NativeMesh {
    Vk::Buffer                 buffer;
    uint32_t                   vertexCount = 0;
    // Stream-agnostic record: the same NativeMesh backs position, frame,
    // surface, index and meshlet buffers, so the address stays type-erased
    // here and gains its element type at the point of use (Cast<>).
    Vk::DeviceAddress<>        vboAddress;
    Vk::AccelerationStructure  blas;
    Vk::DeviceAddress<>        blasAddress;
    Vk::Buffer                 blasBuffer;

    NativeMesh() = default;
    // Both buffers are taken by value: they are move-only handles, so this is the same
    // single move with the ownership stated in the signature, as TextureManager::Adopt
    // does with its image and view.
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

// Stable registry entry: GPU pipelines are retired by PipelineRegistry, never
// by destruction of the pool slot.
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


// Draw-submission translation: material S/T modes -> common.slang sampler words.
// Kept private so shader packing changes do not become public Material API changes.
[[nodiscard]] auto PackMaterialSamplerAddresses(const MaterialSamplerAddresses& addresses, size_t first) noexcept -> uint32_t;

struct DrawCommand {
    InstanceData         instanceData;
    NativeMaterial*      material;
    NativeMaterial*      prePassMaterial;
    // Everything the frame passes need from the source meshes, copied in as
    // addresses at draw time. Pass recording never dereferences stream
    // records -- and never sees a VkAccelerationStructureKHR handle: the
    // BLAS object stays with NativeMesh, which owns build and retirement,
    // while its address is all a TLAS instance or a skinning dispatch names.
    // (The skinned scratch buffer is the one exception: its BLAS is rebuilt
    // after the skinning dispatch each frame, so its address reaches
    // BuildTLAS through the per-frame SkinnedBlasEntry list instead.)
    Vk::DeviceAddress<>                    blasAddress;
    Vk::DeviceAddress<VertexPosition>      skinPosAddress;
    Vk::DeviceAddress<VertexTangentFrame>  skinFrameAddress;
    // The skinning shader reads the skin stream as raw uint words.
    Vk::DeviceAddress<uint32_t>            skinDataAddress;
    BufferHandle                           skinnedVertexBuffer;
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

// One rebuilt skinned-scratch BLAS: which scratch buffer, and the address
// its TLAS instance must name this frame. Produced by the skinning stage,
// consumed by BuildTLAS -- the ordering between them is carried by this
// data, not by a convention about which Impl method runs first.
struct SkinnedBlasEntry {
    BufferHandle        scratch;
    Vk::DeviceAddress<> address;
};

// (Re)builds the BLAS of one skinned scratch buffer from the vertex stream
// the skinning dispatch just wrote. Creates the backing buffer and the
// acceleration structure on first use and keeps them on the scratch mesh;
// later frames rebuild in place. Returns the BLAS address for the caller's
// SkinnedBlasEntry.
//
// A free function, deliberately: everything it touches arrives as a
// parameter, and every failure -- unsupported ray tracing, allocation
// failure, a failed create -- comes back as an error instead of being
// swallowed. On failure during first creation the half-created state is
// rolled back; on failure of a later rebuild the previous BLAS (and the
// address the caller already has) remains intact.
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
