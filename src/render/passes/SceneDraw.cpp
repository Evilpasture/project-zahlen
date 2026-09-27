// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/SceneDraw.hpp"
#include <Zahlen/Profiler.hpp>
#include <cstring>

namespace ZHLN::Passes {

auto IsVisibleIn(DrawFlags flags, RenderPassType passType) noexcept -> bool {
    using enum DrawFlags;
    const bool hasMain   = (flags & VisibleInMain) != None;
    const bool hasShadow = (flags & VisibleInShadow) != None;

    if (!hasMain && !hasShadow) {
        return true;
    }

    return (passType == RenderPassType::Main) ? hasMain : hasShadow;
}

auto UseMeshPath(const DrawCommand& drawCmd, VkPipeline pipelineOverride, bool meshShadingActive) noexcept -> bool {
    return meshShadingActive && pipelineOverride == VK_NULL_HANDLE && drawCmd.material != nullptr && drawCmd.material->HasMeshPipeline() &&
           drawCmd.instanceData.meshletCount > 0;
}

void DrawCSGMeshes(const FrameRecorder& recorder, VkExtent3D extent) noexcept {
    VkCommandBuffer cmd = recorder.cmd;
    auto&           ctx = recorder.ctx;

    auto* const stencilWritePipeline = ctx.csgWritePipeline.Get();
    if (ctx.queues.CsgDraws().empty() || stencilWritePipeline == VK_NULL_HANDLE) {
        return;
    }

    ZHLN::ScopedTimer profTimer("GPU Stencil CSG Passes");

    for (const auto& csgCmd: ctx.queues.CsgDraws()) {
        Vk::ClearStencilAttachment(cmd, {.width = extent.width, .height = extent.height});

        for (const auto& cutter: csgCmd.cutters) {
            const RenderContext::Impl::ObjectConstants push = {.instanceId = cutter.instanceIdx, .isShadowPass = 0};
            SubmitDrawInstanced(recorder.encoder, cutter.draw, cutter.instanceIdx, push, ctx.MeshShadingActive(), stencilWritePipeline, ctx.csgPipelineLayout);
        }

        auto activePipeline = ctx.csgDifferencePipeline.Get();
        if (!csgCmd.cutters.empty() && csgCmd.cutters[0].operation == CSGOperation::Intersection) {
            activePipeline = ctx.csgIntersectionPipeline.Get();
        }
        if (activePipeline == VK_NULL_HANDLE) {
            return;
        }

        const RenderContext::Impl::ObjectConstants push = {.instanceId = csgCmd.eyeInstanceIdx, .isShadowPass = 0};
        SubmitDrawInstanced(recorder.encoder, csgCmd.eyeDraw, csgCmd.eyeInstanceIdx, push, ctx.MeshShadingActive(), activePipeline, ctx.csgPipelineLayout);
    }
}

void Draw3DParticles(const FrameRecorder& recorder) noexcept {
    auto& ctx = recorder.ctx;
    if (!ctx.meshParticleRenderPipeline.Valid() || ctx.queues.MeshParticleEmitters().empty()) {
        return;
    }

    for (const auto& emitter: ctx.queues.MeshParticleEmitters()) {
        auto* pBuf    = ctx.geometry.Resolve(emitter.gpuBuffer).value_or(nullptr);
        const Mesh*     gpuMesh = ctx.geometry.FindMesh(emitter.meshAsset);
        const Material* gpuMat  = ctx.geometry.FindMaterial(emitter.materialAsset);

        if ((pBuf == nullptr) || gpuMesh == nullptr || gpuMat == nullptr) {
            continue;
        }

        auto* posMesh  = ctx.geometry.Resolve(gpuMesh->posBuffer).value_or(nullptr);
        auto* attrMesh = ctx.geometry.Resolve(gpuMesh->attrBuffer).value_or(nullptr);
        auto* iboMesh  = (gpuMesh->indexBuffer != BufferHandle::Invalid) ? ctx.geometry.Resolve(gpuMesh->indexBuffer).value_or(nullptr) : nullptr;

        RenderContext::Impl::MeshParticleRenderPush rpc = {
            .particleBufferAddr = ctx.BufferAddress(pBuf->buffer.Handle()),
            .posAddress         = (posMesh != nullptr) ? posMesh->vboAddress : 0,
            .attrAddress        = (attrMesh != nullptr) ? attrMesh->vboAddress : 0,
            .iboAddress         = (iboMesh != nullptr) ? iboMesh->vboAddress : 0,
            .baseColorFactor    = {},
            .emissiveFactor     = {},
            .indexCount         = gpuMesh->indexCount,
            .albedoIdx          = ctx.textureManager.GetBindlessIndex(gpuMat->albedoMap),
            .normalIdx          = ctx.textureManager.GetBindlessIndex(gpuMat->normalMap),
            .pbrIdx             = ctx.textureManager.GetBindlessIndex(gpuMat->pbrMap),
            .emissiveIdx        = ctx.textureManager.GetBindlessIndex(gpuMat->emissiveMap),
            .roughness          = gpuMat->roughnessFactor,
            .metallic           = gpuMat->metallicFactor,
            .alphaCutoff        = gpuMat->alphaCutoff,
            .alphaMode          = gpuMat->alphaMode,
            ._padding           = 0
        };
        std::memcpy(rpc.baseColorFactor, gpuMat->baseColorFactor, sizeof(float) * 4);
        std::memcpy(rpc.emissiveFactor, gpuMat->emissiveFactor, sizeof(float) * 4);

        uint32_t drawVertexCount = (iboMesh != nullptr) ? gpuMesh->indexCount : gpuMesh->vertexCount;

        recorder.encoder.DrawInstanced<Shaders::Modules::MeshParticleRenderVS>(
            {.pipeline      = ctx.meshParticleRenderPipeline.Get(),
             .layout        = ctx.meshParticleRenderLayout,
             .heap          = true,
             .vertexCount   = drawVertexCount,
             .instanceCount = emitter.maxParticles,
             .firstVertex   = 0,
             .firstInstance = 0},
            rpc
        );
    }
}

void Draw3DParticleShadows(const FrameRecorder& recorder) noexcept {
    auto& ctx = recorder.ctx;
    if (!ctx.meshParticleShadowPipeline.Valid() || ctx.queues.MeshParticleEmitters().empty()) {
        return;
    }

    for (const auto& emitter: ctx.queues.MeshParticleEmitters()) {
        auto* pBuf    = ctx.geometry.Resolve(emitter.gpuBuffer).value_or(nullptr);
        const Mesh*     gpuMesh = ctx.geometry.FindMesh(emitter.meshAsset);
        const Material* gpuMat  = ctx.geometry.FindMaterial(emitter.materialAsset);

        if ((pBuf == nullptr) || gpuMesh == nullptr || gpuMat == nullptr) {
            continue;
        }

        auto* posMesh = ctx.geometry.Resolve(gpuMesh->posBuffer).value_or(nullptr);
        auto* iboMesh = (gpuMesh->indexBuffer != BufferHandle::Invalid) ? ctx.geometry.Resolve(gpuMesh->indexBuffer).value_or(nullptr) : nullptr;

        RenderContext::Impl::MeshParticleRenderPush rpc = {
            .particleBufferAddr = ctx.BufferAddress(pBuf->buffer.Handle()),
            .posAddress         = (posMesh != nullptr) ? posMesh->vboAddress : 0,
            .attrAddress        = 0,
            .iboAddress         = (iboMesh != nullptr) ? iboMesh->vboAddress : 0,
            .baseColorFactor    = {},
            .emissiveFactor     = {},
            .indexCount         = gpuMesh->indexCount,
            .albedoIdx          = ctx.textureManager.GetBindlessIndex(gpuMat->albedoMap),
            .normalIdx          = 0,
            .pbrIdx             = 0,
            .emissiveIdx        = 0,
            .roughness          = 0.0f,
            .metallic           = 0.0f,
            .alphaCutoff        = gpuMat->alphaCutoff,
            .alphaMode          = gpuMat->alphaMode,
            ._padding           = 0
        };
        std::memcpy(rpc.baseColorFactor, gpuMat->baseColorFactor, sizeof(float) * 4);

        uint32_t drawVertexCount = (iboMesh != nullptr) ? gpuMesh->indexCount : gpuMesh->vertexCount;

        recorder.encoder.DrawInstanced<Shaders::Modules::MeshParticleShadowVS>(
            {.pipeline      = ctx.meshParticleShadowPipeline.Get(),
             .layout        = ctx.meshParticleRenderLayout,
             .heap          = true,
             .vertexCount   = drawVertexCount,
             .instanceCount = emitter.maxParticles,
             .firstVertex   = 0,
             .firstInstance = 0},
            rpc, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
        );
    }
}

auto GBufferSceneTargets(RenderContext::Impl& impl) noexcept -> GBufferTargets {
    return GBufferTargets {
        .sceneColor = Vk::Assume<Vk::ColorWrite<Res_SceneColor>>(impl.graphResources.sceneColor),
        .velocity   = Vk::Assume<Vk::ColorWrite<Res_Velocity>>(impl.graphResources.velocityBuffer),
        .normRough  = Vk::Assume<Vk::ColorWrite<Res_NormRough>>(impl.graphResources.normalRoughnessBuffer),
        .emissive   = Vk::Assume<Vk::ColorWrite<Res_Emissive>>(impl.graphResources.emissiveBuffer),
        .clearcoat  = Vk::Assume<Vk::ColorWrite<Res_Clearcoat>>(impl.graphResources.clearcoatBuffer),
        .depth      = Vk::Assume<Vk::DepthStencilWrite<Res_Depth>>(impl.ActivePresentation().depthTarget)
    };
}

auto BuildGroupRanges(const RenderContext::Impl& impl) -> ZHLN::Array<GroupRange> {
    const auto   drawCount = static_cast<uint32_t>(impl.queues.Draws().size());
    ZHLN::Array<GroupRange> groups;
    groups.reserve((drawCount + 15) / 16);

    VkPipeline currentPipeline = VK_NULL_HANDLE;

    for (uint32_t i = 0; i < drawCount; ++i) {
        const auto&       drawCmd = impl.queues.Draws()[i];
        const auto* const drawMat = drawCmd.material;

        if (IsForwardOnly(drawCmd.instanceData.flags) || (drawCmd.flags & DrawFlags::Viewmodel) != DrawFlags::None || !drawMat->pipeline.Valid()) {
            currentPipeline = VK_NULL_HANDLE;
            continue;
        }

        if (i == 0 || drawMat->pipeline.Get() != currentPipeline) {
            groups.push_back(GroupRange {.material = drawMat, .start = i, .count = 1});
            currentPipeline = drawMat->pipeline.Get();
        } else {
            groups.back().count++;
        }
    }

    return groups;
}

void StampScenePass(RenderContext::Impl::ScenePassStamp& stamp, const RenderContext::Impl& ctx, uint32_t drawCount, bool ran) noexcept {
    stamp.draws         = drawCount;
    stamp.csgDraws      = static_cast<uint32_t>(ctx.queues.CsgDraws().size());
    stamp.meshParticles = static_cast<uint32_t>(ctx.queues.MeshParticleEmitters().size());
    stamp.ran           = ran;
    stamp.meshShading   = ctx.MeshShadingActive();
    stamp.gpuCulling    = false;
}

}
