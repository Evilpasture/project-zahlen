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

namespace ZHLN {

namespace {

[[nodiscard]] inline std::array<float, 4> UnpackMorphWeights(const float* weights) noexcept {
    if (weights == nullptr) {
        return {0.0f, 0.0f, 0.0f, 0.0f};
    }
    return {weights[0], weights[1], weights[2], weights[3]};
}

} // namespace

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

namespace Renderer {

void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh, const DrawParams& params) {
    using enum DrawFlags;
    using enum BufferHandle;
    auto* impl = ctx.GetImpl();

    auto posMesh_res        = impl->meshPool.Resolve(mesh.posBuffer);
    auto attrMesh_res       = impl->meshPool.Resolve(mesh.attrBuffer);
    auto nativeMaterial_res = impl->materialPool.Resolve(material.pipeline);

    if (!posMesh_res || !attrMesh_res || !nativeMaterial_res) [[unlikely]] {
        static uint32_t s_WarnCount = 0;
        if (s_WarnCount++ < 5) {
            ZHLN::Log("WARNING: Renderer::Draw skipped draw call with invalid mesh or material handle.");
        }
        return;
    }

    auto* posMesh        = posMesh_res.value();
    auto* attrMesh       = attrMesh_res.value();
    auto* nativeMaterial = nativeMaterial_res.value();

    NativeMaterial* prePassMaterial = nullptr;
    if (material.prePassPipeline != PipelineHandle::Invalid) {
        prePassMaterial = impl->materialPool.Resolve(material.prePassPipeline).value_or(nullptr);
    }

    auto* skinMesh        = (mesh.skinBuffer != Invalid) ? impl->meshPool.Resolve(mesh.skinBuffer).value_or(nullptr) : nullptr;
    auto* nativeIndexMesh = (mesh.indexBuffer != Invalid) ? impl->meshPool.Resolve(mesh.indexBuffer).value_or(nullptr) : nullptr;

    if (params.skinnedVertexBuffer != Invalid) {
        impl->hasSkinnedThisFrame = true;
    }

    auto* finalPosMesh = (params.skinnedVertexBuffer != Invalid) ? impl->meshPool.Resolve(params.skinnedVertexBuffer).value_or(nullptr) : posMesh;

    VkDeviceAddress posAddr  = (finalPosMesh != nullptr) ? finalPosMesh->vboAddress : 0;
    VkDeviceAddress attrAddr = (attrMesh != nullptr) ? attrMesh->vboAddress : 0;

    if (posMesh == attrMesh && posMesh != nullptr) {
        attrAddr = posMesh->vboAddress + (RenderContext::Impl::kMaxLineVertices * sizeof(VertexPosition));
    } else if (params.skinnedVertexBuffer != Invalid && posMesh != nullptr) {
        attrAddr = finalPosMesh->vboAddress + (posMesh->vertexCount * sizeof(VertexPosition));
    }

    // --- BUG FIX: Properly route default texture indices based on handle validity ---
    uint32_t albedoIdx   = material.albedoMap != TextureHandle::Invalid ? impl->textureManager.GetBindlessIndex(material.albedoMap) : 1;
    uint32_t normalIdx   = material.normalMap != TextureHandle::Invalid ? impl->textureManager.GetBindlessIndex(material.normalMap) : 2;
    uint32_t pbrIdx      = material.pbrMap != TextureHandle::Invalid ? impl->textureManager.GetBindlessIndex(material.pbrMap) : 1;
    uint32_t emissiveIdx = material.emissiveMap != TextureHandle::Invalid ? impl->textureManager.GetBindlessIndex(material.emissiveMap) : 0;

    uint32_t isViewmodel      = ((params.flags & DrawFlags::Viewmodel) != DrawFlags::None) ? 1u : 0u;
    uint32_t isSkinned        = (params.skinnedVertexBuffer == Invalid && (params.flags & Skinned) != None) ? 1u : 0u;
    uint32_t activeMorphCount = (params.skinnedVertexBuffer != Invalid) ? 0 : params.activeMorphCount;

    impl->queues.drawQueue.push_back(
        {.instanceData =
             {
                 .world            = params.transform,
                 .prevWorld        = params.prevTransform,
                 .posAddress       = posAddr,
                 .attrAddress      = attrAddr,
                 .skinAddress      = (skinMesh != nullptr) ? skinMesh->vboAddress : 0,
                 .iboAddress       = (nativeIndexMesh != nullptr) ? nativeIndexMesh->vboAddress : 0,
                 .vertexCount      = (posMesh != nullptr) ? posMesh->vertexCount : 0,
                 .indexCount       = mesh.indexCount,
                 .texIndices0      = (normalIdx << 16) | (albedoIdx & 0xFFFF),
                 .texIndices1      = (emissiveIdx << 16) | (pbrIdx & 0xFFFF),
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
                 .morphWeights     = UnpackMorphWeights(params.morphWeights),
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
         .material            = nativeMaterial,
         .prePassMaterial     = prePassMaterial,
         .posMesh             = posMesh,
         .attrMesh            = attrMesh,
         .skinMesh            = skinMesh,
         .skinnedVertexBuffer = params.skinnedVertexBuffer,
         .jointOffset         = params.jointOffset,
         .morphOffset         = params.morphOffset,
         .activeMorphCount    = params.activeMorphCount,
         .morphWeights        = UnpackMorphWeights(params.morphWeights),
         .flags               = params.flags}
    );
}

void DrawCSG(RenderContext& ctx, const Material& eyeMaterial, const Mesh& eyeMesh, const CSGDrawParams& params) {
    auto* impl = ctx.GetImpl();

    auto MakeCommand = [&](const Material& material, const Mesh& mesh, const JPH::Mat44& transform, const JPH::Mat44& prevTransform, float cullRadius,
                           uint32_t jointOffset, BufferHandle skinnedVertexBuffer, DrawFlags flags) -> DrawCommand {
        auto posMesh_res        = impl->meshPool.Resolve(mesh.posBuffer);
        auto attrMesh_res       = impl->meshPool.Resolve(mesh.attrBuffer);
        auto nativeMaterial_res = impl->materialPool.Resolve(material.pipeline);

        if (!posMesh_res || !attrMesh_res || !nativeMaterial_res) {
            return {};
        }

        auto* posMesh        = posMesh_res.value();
        auto* attrMesh       = attrMesh_res.value();
        auto* nativeMaterial = nativeMaterial_res.value();

        NativeMaterial* prePassMaterial = nullptr;
        if (material.prePassPipeline != PipelineHandle::Invalid) {
            prePassMaterial = impl->materialPool.Resolve(material.prePassPipeline).value_or(nullptr);
        }

        auto* skinMesh  = (mesh.skinBuffer != BufferHandle::Invalid) ? impl->meshPool.Resolve(mesh.skinBuffer).value_or(nullptr) : nullptr;
        auto* indexMesh = (mesh.indexBuffer != BufferHandle::Invalid) ? impl->meshPool.Resolve(mesh.indexBuffer).value_or(nullptr) : nullptr;

        auto* finalPosMesh = (skinnedVertexBuffer != BufferHandle::Invalid) ? impl->meshPool.Resolve(skinnedVertexBuffer).value_or(nullptr) : posMesh;

        VkDeviceAddress posAddr  = (finalPosMesh != nullptr) ? finalPosMesh->vboAddress : 0;
        VkDeviceAddress attrAddr = (attrMesh != nullptr) ? attrMesh->vboAddress : 0;

        if (posMesh == attrMesh && posMesh != nullptr) {
            attrAddr = finalPosMesh->vboAddress + (500000 * sizeof(VertexPosition));
        } else if (skinnedVertexBuffer != BufferHandle::Invalid && finalPosMesh != nullptr) {
            attrAddr = finalPosMesh->vboAddress + (finalPosMesh->vertexCount * sizeof(VertexPosition));
        }

        // --- BUG FIX: Properly route default texture indices based on handle validity ---
        uint32_t albedoIdx   = material.albedoMap != TextureHandle::Invalid ? impl->textureManager.GetBindlessIndex(material.albedoMap) : 1;
        uint32_t normalIdx   = material.normalMap != TextureHandle::Invalid ? impl->textureManager.GetBindlessIndex(material.normalMap) : 2;
        uint32_t pbrIdx      = material.pbrMap != TextureHandle::Invalid ? impl->textureManager.GetBindlessIndex(material.pbrMap) : 1;
        uint32_t emissiveIdx = material.emissiveMap != TextureHandle::Invalid ? impl->textureManager.GetBindlessIndex(material.emissiveMap) : 0;

        uint32_t isSkinned = (skinnedVertexBuffer == BufferHandle::Invalid && (flags & DrawFlags::Skinned) != DrawFlags::None) ? 1u : 0u;

        return {
            .instanceData =
                {
                    .world            = transform,
                    .prevWorld        = prevTransform,
                    .posAddress       = posAddr,
                    .attrAddress      = attrAddr,
                    .skinAddress      = (skinMesh != nullptr) ? skinMesh->vboAddress : 0,
                    .iboAddress       = (indexMesh != nullptr) ? indexMesh->vboAddress : 0,
                    .vertexCount      = (finalPosMesh != nullptr) ? finalPosMesh->vertexCount : 0,
                    .indexCount       = mesh.indexCount,
                    .texIndices0      = (normalIdx << 16) | (albedoIdx & 0xFFFF),
                    .texIndices1      = (emissiveIdx << 16) | (pbrIdx & 0xFFFF),
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
            .material            = nativeMaterial,
            .prePassMaterial     = prePassMaterial,
            .posMesh             = finalPosMesh,
            .attrMesh            = attrMesh,
            .skinMesh            = skinMesh,
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

    impl->queues.csgDrawQueue.push_back(std::move(csgCmd));
}

void DrawDecal(RenderContext& ctx, const DecalParams& params) {
    auto* impl = ctx.GetImpl();

    // --- BUG FIX: Safely route valid indices directly for Decals ---
    impl->queues.decalQueue.push_back(
        {.transform    = params.transform,
         .invTransform = params.invTransform,
         .albedoIndex  = params.albedoMap != TextureHandle::Invalid ? impl->textureManager.GetBindlessIndex(params.albedoMap) : 1,
         .normalIndex  = params.normalMap != TextureHandle::Invalid ? impl->textureManager.GetBindlessIndex(params.normalMap) : 2,
         .roughness    = params.roughness,
         .metallic     = params.metallic}
    );
}

} // namespace Renderer
} // namespace ZHLN
