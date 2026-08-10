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
};

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
        .emissive = (material.emissiveMap != TextureHandle::Invalid) ? impl->textureManager.GetBindlessIndex(material.emissiveMap) : 0
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

    for (uint32_t i = 0; i < drawCount; ++i) {
        sortDrawQueueScratch[i] = queues.drawQueue[sortItemsScratch[i].payload];
    }

    queues.drawQueue = sortDrawQueueScratch;
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

    dst[lineInstanceIdx] = {
        .world            = JPH::Mat44::sIdentity(),
        .prevWorld        = JPH::Mat44::sIdentity(),
        .posAddress       = posAddr,
        .attrAddress      = attrAddr,
        .skinAddress      = 0,
        .iboAddress       = 0,
        .vertexCount      = vertIdx,
        .indexCount       = 0,
        .texIndices0      = (2 << 16) | 1,
        .texIndices1      = (1 << 16) | 0,
        .cullRadius       = 10000.0f,
        .metallicFactor   = 0.0f,
        .roughnessFactor  = 1.0f,
        .alphaCutoff      = 0.0f,
        .flags            = 2,
        .jointOffset      = 0,
        .morphOffset      = 0,
        .activeMorphCount = 0,
        .localCenter      = {0.0f, 0.0f, 0.0f},
        ._paddingCenter   = 0,
        .morphWeights     = {0.0f, 0.0f, 0.0f, 0.0f},
        .baseColorFactor  = {1.0f, 1.0f, 1.0f, 1.0f},
        .emissiveFactor   = {0.0f, 0.0f, 0.0f, 1.0f},
    };

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
        {.instanceData =
             {
                 .world            = params.transform,
                 .prevWorld        = params.prevTransform,
                 .posAddress       = resolved->posAddr,
                 .attrAddress      = resolved->attrAddr,
                 .skinAddress      = (resolved->skinMesh != nullptr) ? resolved->skinMesh->vboAddress : 0,
                 .iboAddress       = (resolved->indexMesh != nullptr) ? resolved->indexMesh->vboAddress : 0,
                 .vertexCount      = (resolved->posMesh != nullptr) ? resolved->posMesh->vertexCount : 0,
                 .indexCount       = mesh.indexCount,
                 .texIndices0      = (tex.normal << 16) | (tex.albedo & 0xFFFF),
                 .texIndices1      = (tex.emissive << 16) | (tex.pbr & 0xFFFF),
                 .cullRadius       = params.cullRadius,
                 .metallicFactor   = params.metallic >= 0.0f ? params.metallic : material.metallicFactor,
                 .roughnessFactor  = params.roughness >= 0.0f ? params.roughness : material.roughnessFactor,
                 .alphaCutoff      = material.alphaCutoff,
                 .flags            = (isViewmodel << 16) | (isSkinned << 8) | (material.alphaMode & 0xFF),
                 .jointOffset      = params.jointOffset,
                 .morphOffset      = params.morphOffset,
                 .activeMorphCount = activeMorphCount,
                 .localCenter      = {params.localCenter[0], params.localCenter[1], params.localCenter[2]},
                 ._paddingCenter   = {},
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
             },
         .material            = resolved->material,
         .prePassMaterial     = resolved->prePassMaterial,
         .posMesh             = resolved->finalPosMesh,
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
                {
                    .world            = transform,
                    .prevWorld        = prevTransform,
                    .posAddress       = resolved->posAddr,
                    .attrAddress      = resolved->attrAddr,
                    .skinAddress      = (resolved->skinMesh != nullptr) ? resolved->skinMesh->vboAddress : 0,
                    .iboAddress       = (resolved->indexMesh != nullptr) ? resolved->indexMesh->vboAddress : 0,
                    .vertexCount      = (resolved->finalPosMesh != nullptr) ? resolved->finalPosMesh->vertexCount : 0,
                    .indexCount       = mesh.indexCount,
                    .texIndices0      = (tex.normal << 16) | (tex.albedo & 0xFFFF),
                    .texIndices1      = (tex.emissive << 16) | (tex.pbr & 0xFFFF),
                    .cullRadius       = cullRadius,
                    .metallicFactor   = material.metallicFactor,
                    .roughnessFactor  = material.roughnessFactor,
                    .alphaCutoff      = material.alphaCutoff,
                    .flags            = (isSkinned << 8) | (material.alphaMode & 0xFF),
                    .jointOffset      = jointOffset,
                    .morphOffset      = 0,
                    .activeMorphCount = 0,
                    .localCenter      = {},
                    ._paddingCenter   = {},
                    .morphWeights     = {},
                    .baseColorFactor  = {material.baseColorFactor[0], material.baseColorFactor[1], material.baseColorFactor[2], material.baseColorFactor[3]},
                    .emissiveFactor   = {material.emissiveFactor[0], material.emissiveFactor[1], material.emissiveFactor[2], material.emissiveFactor[3]},
                },
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
