// src/engine/RenderDrawCommands.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "Zahlen/Math3D.hpp"
#include <Zahlen/Core/RadixSort.hpp>
#include <Zahlen/Render.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>

namespace ZHLN {

namespace {

struct ResolvedMeshMaterial {
    NativeMesh*     posMesh         = nullptr;
    NativeMesh*     attrMesh        = nullptr;
    NativeMesh*     finalPosMesh    = nullptr;
    NativeMesh*     skinMesh        = nullptr;
    NativeMesh*     indexMesh       = nullptr;
    NativeMaterial* material        = nullptr;
    NativeMaterial* prePassMaterial = nullptr;
    VkDeviceAddress posAddr         = 0;
    VkDeviceAddress attrAddr        = 0;

    // VK_EXT_mesh_shader streams (0 / 0 when the mesh has no meshlets, which
    // makes both the task shader and the CPU-side path fall back to vertices).
    VkDeviceAddress meshletAddr       = 0;
    VkDeviceAddress meshletVertexAddr = 0;
    VkDeviceAddress meshletTriAddr    = 0;
    uint32_t        meshletCount      = 0;
};

/// Meshlet streams describe the ORIGINAL vertex pool. A GPU-skinned draw
/// renders from a separate, post-skinning vertex buffer, so its meshlet vertex
/// indices would no longer line up: those draws keep the vertex pipeline.
[[nodiscard]] inline bool MeshletsUsable(const Mesh& mesh, BufferHandle skinnedVertexBuffer) noexcept {
    return mesh.meshletCount > 0 && mesh.meshletBuffer != BufferHandle::Invalid && mesh.meshletVertexBuffer != BufferHandle::Invalid &&
           mesh.meshletTriBuffer != BufferHandle::Invalid && skinnedVertexBuffer == BufferHandle::Invalid;
}

struct BindlessIndices {
    uint32_t albedo;
    uint32_t normal;
    uint32_t pbr;
    uint32_t emissive;
};

[[nodiscard]] inline std::array<float, 4> UnpackMorphWeights(const float* weights) noexcept {
    if (weights == nullptr) {
        return {0.0f, 0.0f, 0.0f, 0.0f};
    }
    return {weights[0], weights[1], weights[2], weights[3]};
}

[[nodiscard]] BindlessIndices ResolveMaterialTextures(RenderContext::Impl* impl, const Material& material) noexcept {
    return {
        .albedo   = (material.albedoMap != TextureHandle::Invalid) ? impl->textureManager.GetBindlessIndex(material.albedoMap) : 1,
        .normal   = (material.normalMap != TextureHandle::Invalid) ? impl->textureManager.GetBindlessIndex(material.normalMap) : 2,
        .pbr      = (material.pbrMap != TextureHandle::Invalid) ? impl->textureManager.GetBindlessIndex(material.pbrMap) : 1,
        .emissive = (material.emissiveMap != TextureHandle::Invalid) ? impl->textureManager.GetBindlessIndex(material.emissiveMap) : 1
    };
}

/// Inputs for one GPU instance record. `resolved` may be null: the line queue
/// owns its vertex buffers itself and has no mesh material, so it contributes no
/// skin / IBO / meshlet addresses.
struct InstanceDataDesc {
    const ResolvedMeshMaterial* resolved = nullptr;

    JPH::Mat44 world     = JPH::Mat44::sIdentity();
    JPH::Mat44 prevWorld = JPH::Mat44::sIdentity();

    /// The line queue points at its own position/attribute pair; mesh draws take
    /// theirs from the resolved mesh.
    uint64_t posAddress  = 0;
    uint64_t attrAddress = 0;

    BindlessIndices indices {};

    /// Mirrors `Material::alphaMode`: 0 opaque, 1 masked, 2 blend.
    uint32_t alphaMode   = 0;
    bool     isViewmodel = false;
    bool     isSkinned   = false;

    uint32_t vertexCount      = 0;
    uint32_t indexCount       = 0;
    uint32_t jointOffset      = 0;
    uint32_t morphOffset      = 0;
    uint32_t activeMorphCount = 0;

    float cullRadius      = 0.0f;
    float metallicFactor  = 0.0f;
    float roughnessFactor = 1.0f;
    float alphaCutoff     = 0.0f;

    std::array<float, 3> localCenter     = {};
    std::array<float, 4> morphWeights    = {};
    std::array<float, 4> baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> emissiveFactor  = {0.0f, 0.0f, 0.0f, 1.0f};
};

/**
 * @brief Pack an `InstanceDataDesc` into the GPU instance record.
 *
 * The bit-packing (the two texture-index pairs, the viewmodel/skinned/alpha-mode
 * flag word), the skin/IBO/meshlet address derivation and the two padding fields
 * lived in three copies; this is the single place that knows them. The counts and
 * the position/attribute addresses stay explicit because the call sites
 * legitimately disagree: the line queue has no indices at all, and the CSG path
 * draws `finalPosMesh` rather than `posMesh`.
 */
[[nodiscard]] inline auto BuildGPUInstanceData(const InstanceDataDesc& desc) noexcept -> InstanceData {
    const ResolvedMeshMaterial* res = desc.resolved;

    const uint32_t isViewmodel = desc.isViewmodel ? 1u : 0u;
    const uint32_t isSkinned   = desc.isSkinned ? 1u : 0u;

    return InstanceData {
        .world            = desc.world,
        .prevWorld        = desc.prevWorld,
        .posAddress       = desc.posAddress,
        .attrAddress      = desc.attrAddress,
        .skinAddress      = (res != nullptr && res->skinMesh != nullptr) ? res->skinMesh->vboAddress : 0ull,
        .iboAddress       = (res != nullptr && res->indexMesh != nullptr) ? res->indexMesh->vboAddress : 0ull,
        .vertexCount      = desc.vertexCount,
        .indexCount       = desc.indexCount,
        .texIndices0      = (desc.indices.normal << 16) | (desc.indices.albedo & 0xFFFFu),
        .texIndices1      = (desc.indices.emissive << 16) | (desc.indices.pbr & 0xFFFFu),
        .cullRadius       = desc.cullRadius,
        .metallicFactor   = desc.metallicFactor,
        .roughnessFactor  = desc.roughnessFactor,
        .alphaCutoff      = desc.alphaCutoff,
        .flags            = (isViewmodel << 16) | (isSkinned << 8) | (desc.alphaMode & 0xFFu),
        .jointOffset      = desc.jointOffset,
        .morphOffset      = desc.morphOffset,
        .activeMorphCount = desc.activeMorphCount,
        .localCenter      = desc.localCenter,
        ._paddingCenter   = 0,
        .morphWeights     = desc.morphWeights,
        .baseColorFactor  = desc.baseColorFactor,
        .emissiveFactor   = desc.emissiveFactor,
        // VK_EXT_mesh_shader streams (all zero => vertex pipeline). Debug lines
        // are not meshletized: LINE_LIST topology has no mesh pipeline variant.
        .meshletAddress       = (res != nullptr) ? res->meshletAddr : 0ull,
        .meshletVertexAddress = (res != nullptr) ? res->meshletVertexAddr : 0ull,
        .meshletTriAddress    = (res != nullptr) ? res->meshletTriAddr : 0ull,
        .meshletCount         = (res != nullptr) ? res->meshletCount : 0u,
        ._paddingMeshlet      = 0,
    };
}

[[nodiscard]] std::optional<ResolvedMeshMaterial>
    ResolveDrawInputs(RenderContext::Impl* impl, const Material& material, const Mesh& mesh, BufferHandle skinnedVertexBuffer) noexcept {
    using enum BufferHandle;

    auto posMesh_res        = impl->meshPool.Resolve(mesh.posBuffer);
    auto attrMesh_res       = impl->meshPool.Resolve(mesh.attrBuffer);
    auto nativeMaterial_res = impl->materialPool.Resolve(material.pipeline);

    if (!posMesh_res || !attrMesh_res || !nativeMaterial_res) [[unlikely]] {
        return std::nullopt;
    }

    ResolvedMeshMaterial res;
    res.posMesh  = posMesh_res.value();
    res.attrMesh = attrMesh_res.value();
    res.material = nativeMaterial_res.value();

    if (material.prePassPipeline != PipelineHandle::Invalid) {
        res.prePassMaterial = impl->materialPool.Resolve(material.prePassPipeline).value_or(nullptr);
    }

    res.skinMesh  = (mesh.skinBuffer != Invalid) ? impl->meshPool.Resolve(mesh.skinBuffer).value_or(nullptr) : nullptr;
    res.indexMesh = (mesh.indexBuffer != Invalid) ? impl->meshPool.Resolve(mesh.indexBuffer).value_or(nullptr) : nullptr;

    res.finalPosMesh = (skinnedVertexBuffer != Invalid) ? impl->meshPool.Resolve(skinnedVertexBuffer).value_or(nullptr) : res.posMesh;

    res.posAddr  = (res.finalPosMesh != nullptr) ? res.finalPosMesh->vboAddress : 0;
    res.attrAddr = (res.attrMesh != nullptr) ? res.attrMesh->vboAddress : 0;

    if (MeshletsUsable(mesh, skinnedVertexBuffer)) {
        auto* meshletMesh = impl->meshPool.Resolve(mesh.meshletBuffer).value_or(nullptr);
        auto* meshletVtx  = impl->meshPool.Resolve(mesh.meshletVertexBuffer).value_or(nullptr);
        auto* meshletTri  = impl->meshPool.Resolve(mesh.meshletTriBuffer).value_or(nullptr);

        if (meshletMesh != nullptr && meshletVtx != nullptr && meshletTri != nullptr) {
            res.meshletAddr       = meshletMesh->vboAddress;
            res.meshletVertexAddr = meshletVtx->vboAddress;
            res.meshletTriAddr    = meshletTri->vboAddress;
            res.meshletCount      = mesh.meshletCount;
        }
    }

    if (res.posMesh == res.attrMesh && res.posMesh != nullptr) {
        res.attrAddr = res.posMesh->vboAddress + (RenderContext::Impl::kMaxLineVertices * sizeof(VertexPosition));
    } else if (skinnedVertexBuffer != Invalid && res.posMesh != nullptr) {
        res.attrAddr = res.finalPosMesh->vboAddress + (res.posMesh->vertexCount * sizeof(VertexPosition));
    }

    return res;
}

} // namespace

// ============================================================================
// RenderContext::Impl Internal Member Functions
// ============================================================================

void RenderContext::Impl::SortDrawQueue() {
    auto drawCount = static_cast<uint32_t>(queues.drawQueue.size());
    if (drawCount == 0) {
        return;
    }

    sortItemsScratch.resize(drawCount);
    sortTempScratch.resize(drawCount);
    sortDrawQueueScratch.resize(drawCount);

    for (uint32_t i = 0; i < drawCount; ++i) {
        sortItemsScratch[i] = {.key = SortKey::Pack(queues.drawQueue[i].material, queues.drawQueue[i].posMesh), .payload = i};
    }

    RadixSort64(sortItemsScratch.data(), sortTempScratch.data(), drawCount);

    // Gather sorted commands into scratch once, then swap ownership with the
    // queue. The previous assignment copied every DrawCommand a second time and
    // replaced the whole backing allocation.
    for (uint32_t i = 0; i < drawCount; ++i) {
        sortDrawQueueScratch[i] = queues.drawQueue[sortItemsScratch[i].payload];
    }

    queues.drawQueue.swap(sortDrawQueueScratch);
}

void RenderContext::Impl::FlushLineQueue() {
    activeLineVertexCount = 0;

    if (queues.lineQueue.empty() || !linePipeline.Valid()) {
        return;
    }

    constexpr uint32_t maxLineVerts   = kMaxLineVertices;
    uint32_t           totalLineVerts = std::min(static_cast<uint32_t>(queues.lineQueue.size() * 2), maxLineVerts);

    auto  mappedRegion = frames.lineVbos[frame_index].Map();
    auto* basePosPtr   = static_cast<VertexPosition*>(mappedRegion.data);
    auto* baseAttrPtr  = reinterpret_cast<VertexAttributes*>(basePosPtr + maxLineVerts);

    Packed1010102 dummyNorm = Math::PackNormal(0.0f, 1.0f, 0.0f);
    Packed1010102 dummyTang = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f);

    uint32_t vertIdx = 0;
    for (const auto& line: queues.lineQueue) {
        if (vertIdx + 2 > totalLineVerts) {
            break;
        }

        basePosPtr[vertIdx]  = {.position = {line.start.GetX(), line.start.GetY(), line.start.GetZ()}};
        baseAttrPtr[vertIdx] = {
            .normal  = dummyNorm,
            .tangent = dummyTang,
            .uv      = Math::PackUV(0.0f, 0.0f),
            .color   = Math::PackColor(line.colorStart.GetX(), line.colorStart.GetY(), line.colorStart.GetZ(), line.colorStart.GetW())
        };
        vertIdx++;

        basePosPtr[vertIdx]  = {.position = {line.end.GetX(), line.end.GetY(), line.end.GetZ()}};
        baseAttrPtr[vertIdx] = {
            .normal  = dummyNorm,
            .tangent = dummyTang,
            .uv      = Math::PackUV(1.0f, 1.0f),
            .color   = Math::PackColor(line.colorEnd.GetX(), line.colorEnd.GetY(), line.colorEnd.GetZ(), line.colorEnd.GetW())
        };
        vertIdx++;
    }

    activeLineVertexCount = vertIdx;

    auto lineInstanceIdx = static_cast<uint32_t>(queues.drawQueue.size());
    lineInstanceId       = lineInstanceIdx;

    VkDeviceAddress posAddr  = frames.lineVboAddresses[frame_index];
    VkDeviceAddress attrAddr = posAddr + (maxLineVerts * sizeof(VertexPosition));

    auto  mappedInst = frames.instanceDataBuffers[frame_index].Map();
    auto* dst        = static_cast<InstanceData*>(mappedInst.data);

    // Debug lines run on their own position/attribute pair, carry no indices and
    // are not meshletized (resolved == nullptr zeroes skin/IBO/meshlet).
    dst[lineInstanceIdx] = BuildGPUInstanceData(
        InstanceDataDesc {
            .posAddress  = posAddr,
            .attrAddress = attrAddr,
            .indices     = BindlessIndices {.albedo = 1, .normal = 2, .pbr = 0, .emissive = 1},
            .alphaMode   = 2,
            .vertexCount = vertIdx,
            .cullRadius  = 10000.0f,
        }
    );

    queues.lineQueue.clear();
}

// ============================================================================
// RenderContext Public Member Functions
// ============================================================================

void RenderContext::Draw(const Material& material, const Mesh& mesh, const DrawParams& params) noexcept {
    using enum DrawFlags;
    using enum BufferHandle;

    auto resolved = ResolveDrawInputs(_impl.get(), material, mesh, params.skinnedVertexBuffer);
    if (!resolved) [[unlikely]] {
        static uint32_t s_WarnCount = 0;
        if (s_WarnCount++ < 5) {
            ZHLN::Log("WARNING: RenderContext::Draw skipped draw call with invalid mesh or material handle.");
        }
        return;
    }

    if (params.skinnedVertexBuffer != Invalid) {
        _impl->hasSkinnedThisFrame = true;
    }

    auto tex = ResolveMaterialTextures(_impl.get(), material);

    uint32_t isViewmodel      = ((params.flags & DrawFlags::Viewmodel) != DrawFlags::None) ? 1u : 0u;
    uint32_t isSkinned        = (params.skinnedVertexBuffer == Invalid && (params.flags & Skinned) != None) ? 1u : 0u;
    uint32_t activeMorphCount = (params.skinnedVertexBuffer != Invalid) ? 0 : params.activeMorphCount;

    auto morphWeights = UnpackMorphWeights(params.morphWeights);

    _impl->queues.drawQueue.push_back(
        {.instanceData = BuildGPUInstanceData(
             InstanceDataDesc {
                 .resolved  = &*resolved,
                 .world     = params.transform,
                 .prevWorld = params.prevTransform,
                 // posAddress points at scratchMesh for basic.slang.
                 .posAddress       = resolved->posAddr,
                 .attrAddress      = resolved->attrAddr,
                 .indices          = tex,
                 .alphaMode        = static_cast<uint32_t>(material.alphaMode) & 0xFFu,
                 .isViewmodel      = isViewmodel != 0u,
                 .isSkinned        = isSkinned != 0u,
                 .vertexCount      = (resolved->posMesh != nullptr) ? resolved->posMesh->vertexCount : 0u,
                 .indexCount       = mesh.indexCount,
                 .jointOffset      = params.jointOffset,
                 .morphOffset      = params.morphOffset,
                 .activeMorphCount = activeMorphCount,
                 .cullRadius       = params.cullRadius,
                 .metallicFactor   = params.metallic >= 0.0f ? params.metallic : material.metallicFactor,
                 .roughnessFactor  = params.roughness >= 0.0f ? params.roughness : material.roughnessFactor,
                 .alphaCutoff      = material.alphaCutoff,
                 .localCenter      = {params.localCenter[0], params.localCenter[1], params.localCenter[2]},
                 .morphWeights     = morphWeights,
                 .baseColorFactor  = (params.colorOverride[3] >= 0.0f) ?
                                         params.colorOverride :
                                         std::array<float, 4> {
                                             material.baseColorFactor[0], material.baseColorFactor[1], material.baseColorFactor[2], material.baseColorFactor[3]
                                         },
                 .emissiveFactor =
                     (params.emissiveOverride[3] >= 0.0f) ?
                         params.emissiveOverride :
                         std::array<float, 4> {material.emissiveFactor[0], material.emissiveFactor[1], material.emissiveFactor[2], material.emissiveFactor[3]},
             }
         ),
         .material            = resolved->material,
         .prePassMaterial     = resolved->prePassMaterial,
         .posMesh             = resolved->posMesh,
         .attrMesh            = resolved->attrMesh,
         .skinMesh            = resolved->skinMesh,
         .skinnedVertexBuffer = params.skinnedVertexBuffer,
         .jointOffset         = params.jointOffset,
         .morphOffset         = params.morphOffset,
         .activeMorphCount    = params.activeMorphCount,
         .morphWeights        = morphWeights,
         .flags               = params.flags}
    );
}

void RenderContext::DrawCSG(const Material& eyeMaterial, const Mesh& eyeMesh, const CSGDrawParams& params) noexcept {
    auto MakeCommand = [&](const Material& material, const Mesh& mesh, const JPH::Mat44& transform, const JPH::Mat44& prevTransform, float cullRadius,
                           uint32_t jointOffset, BufferHandle skinnedVertexBuffer, DrawFlags flags) -> DrawCommand {
        auto resolved = ResolveDrawInputs(_impl.get(), material, mesh, skinnedVertexBuffer);
        if (!resolved) {
            return {};
        }

        auto tex = ResolveMaterialTextures(_impl.get(), material);

        uint32_t isSkinned = (skinnedVertexBuffer == BufferHandle::Invalid && (flags & DrawFlags::Skinned) != DrawFlags::None) ? 1u : 0u;

        return {
            .instanceData =
                // CSG cutters are stencil-only draws; they still carry the
                // meshlet streams so they can take the mesh path too.
            BuildGPUInstanceData(
                InstanceDataDesc {
                    .resolved        = &*resolved,
                    .world           = transform,
                    .prevWorld       = prevTransform,
                    .posAddress      = resolved->posAddr,
                    .attrAddress     = resolved->attrAddr,
                    .indices         = tex,
                    .alphaMode       = static_cast<uint32_t>(material.alphaMode) & 0xFFu,
                    .isSkinned       = isSkinned != 0u,
                    .vertexCount     = (resolved->finalPosMesh != nullptr) ? resolved->finalPosMesh->vertexCount : 0u,
                    .indexCount      = mesh.indexCount,
                    .jointOffset     = jointOffset,
                    .cullRadius      = cullRadius,
                    .metallicFactor  = material.metallicFactor,
                    .roughnessFactor = material.roughnessFactor,
                    .alphaCutoff     = material.alphaCutoff,
                    .baseColorFactor = {material.baseColorFactor[0], material.baseColorFactor[1], material.baseColorFactor[2], material.baseColorFactor[3]},
                    .emissiveFactor  = {material.emissiveFactor[0], material.emissiveFactor[1], material.emissiveFactor[2], material.emissiveFactor[3]},
                }
            ),
            .material            = resolved->material,
            .prePassMaterial     = resolved->prePassMaterial,
            .posMesh             = resolved->finalPosMesh,
            .attrMesh            = resolved->attrMesh,
            .skinMesh            = resolved->skinMesh,
            .skinnedVertexBuffer = skinnedVertexBuffer,
            .jointOffset         = jointOffset,
            .morphOffset         = 0,
            .activeMorphCount    = 0,
            .morphWeights        = {},
            .flags               = flags
        };
    };

    CSGDrawCommand csgCmd;

    DrawFlags eyeFlags = params.eyeParams.flags;
    csgCmd.eyeDraw     = MakeCommand(
        eyeMaterial, eyeMesh, params.eyeParams.transform, params.eyeParams.prevTransform, params.eyeParams.cullRadius, params.eyeParams.jointOffset,
        params.eyeParams.skinnedVertexBuffer, eyeFlags
    );

    for (const auto& cutter: params.cutters) {
        DrawCommand cutCmd = MakeCommand(
            cutter.material, cutter.mesh, cutter.transform, cutter.prevTransform, cutter.cullRadius, cutter.jointOffset, cutter.skinnedVertexBuffer,
            cutter.flags
        );
        csgCmd.cutters.push_back({.draw = cutCmd, .instanceIdx = 0, .operation = cutter.operation});
    }

    _impl->queues.csgDrawQueue.push_back(std::move(csgCmd));
}

void RenderContext::DrawDecal(const DecalParams& params) noexcept {
    _impl->queues.decalQueue.push_back(
        {.transform    = params.transform,
         .invTransform = params.invTransform,
         .albedoIndex  = params.albedoMap != TextureHandle::Invalid ? _impl->textureManager.GetBindlessIndex(params.albedoMap) : 1,
         .normalIndex  = params.normalMap != TextureHandle::Invalid ? _impl->textureManager.GetBindlessIndex(params.normalMap) : 2,
         .roughness    = params.roughness,
         .metallic     = params.metallic}
    );
}

} // namespace ZHLN
