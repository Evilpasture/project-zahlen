// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/SceneDraw.hpp"
#include <Zahlen/Profiler.hpp>

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

void DrawCSGMeshes(PassContext& passCtx, VkExtent3D extent) noexcept {
    VkCommandBuffer cmd = passCtx.Cmd();
    auto&           ctx = passCtx.ctx;

    auto* const stencilWritePipeline = ctx.csgWritePipeline.Get();
    if (ctx.queues.CsgDraws().empty() || stencilWritePipeline == VK_NULL_HANDLE) {
        return;
    }

    ZHLN::ScopedTimer profTimer("GPU Stencil CSG Passes");

    for (const auto& csgCmd: ctx.queues.CsgDraws()) {
        Vk::ClearStencilAttachment(cmd, {.width = extent.width, .height = extent.height});

        for (const auto& cutter: csgCmd.cutters) {
            const RenderContext::Impl::ObjectConstants push = {.instanceId = cutter.instanceIdx, .isShadowPass = 0};
            SubmitDrawInstanced(passCtx.encoder, cutter.draw, cutter.instanceIdx, push, ctx.MeshShadingActive(), stencilWritePipeline, ctx.csgPipelineLayout);
        }

        auto activePipeline = ctx.csgDifferencePipeline.Get();
        if (!csgCmd.cutters.empty() && csgCmd.cutters[0].operation == CSGOperation::Intersection) {
            activePipeline = ctx.csgIntersectionPipeline.Get();
        }
        if (activePipeline == VK_NULL_HANDLE) {
            return;
        }

        const RenderContext::Impl::ObjectConstants push = {.instanceId = csgCmd.eyeInstanceIdx, .isShadowPass = 0};
        SubmitDrawInstanced(passCtx.encoder, csgCmd.eyeDraw, csgCmd.eyeInstanceIdx, push, ctx.MeshShadingActive(), activePipeline, ctx.csgPipelineLayout);
    }
}

void Draw3DParticles(PassContext& passCtx) noexcept {
    auto& ctx = passCtx.ctx;
    if (!ctx.meshParticleRenderPipeline.Valid() || ctx.queues.MeshParticleEmitters().empty()) {
        return;
    }

    for (const auto& emitter: ctx.queues.MeshParticleEmitters()) {
        auto* pBuf    = ctx.geometry.Resolve(emitter.gpuBuffer);
        const Mesh*     gpuMesh = ctx.geometry.FindMesh(emitter.meshAsset);
        const Material* gpuMat  = ctx.geometry.FindMaterial(emitter.materialAsset);

        if ((pBuf == nullptr) || gpuMesh == nullptr || gpuMat == nullptr) {
            continue;
        }

        auto* posMesh     = ctx.geometry.Resolve(gpuMesh->posBuffer);
        auto* frameMesh   = ctx.geometry.Resolve(gpuMesh->tangentFrameBuffer);
        auto* surfaceMesh = ctx.geometry.Resolve(gpuMesh->surfaceBuffer);
        auto* iboMesh     = (gpuMesh->indexBuffer != BufferHandle::Invalid) ? ctx.geometry.Resolve(gpuMesh->indexBuffer) : nullptr;
        if (posMesh == nullptr) {
            continue;
        }

        RenderContext::Impl::MeshParticleRenderPush rpc = {
            .particleBufferAddr  = ctx.BufferAddress(pBuf->buffer.Handle()),
            .posAddress          = posMesh->vboAddress,
            .baseColorFactor     = gpuMat->baseColorFactor,
            .emissiveFactor      = gpuMat->emissiveFactor,
            .tangentFrameAddress = (frameMesh != nullptr) ? frameMesh->vboAddress : 0,
            .surfaceAddress      = (surfaceMesh != nullptr) ? surfaceMesh->vboAddress : 0,
            .iboAddress          = (iboMesh != nullptr) ? iboMesh->vboAddress : 0,
            .indexCount          = gpuMesh->indexCount,
            .albedoIdx           = ctx.textureManager.GetBindlessIndex(gpuMat->albedoMap),
            .normalIdx           = ctx.textureManager.GetBindlessIndex(gpuMat->normalMap),
            .pbrIdx              = ctx.textureManager.GetBindlessIndex(gpuMat->pbrMap),
            .emissiveIdx         = ctx.textureManager.GetBindlessIndex(gpuMat->emissiveMap),
            .roughness           = gpuMat->roughnessFactor,
            .metallic            = gpuMat->metallicFactor,
            .alphaCutoff         = gpuMat->alphaCutoff,
            .alphaMode           = gpuMat->alphaMode,
            .samplerCodes0       = PackMaterialSamplerAddresses(gpuMat->textureSamplers, 0),
            .samplerCodes1       = PackMaterialSamplerAddresses(gpuMat->textureSamplers, 8),
            .unlit               = gpuMat->unlit ? 1u : 0u
        };

        uint32_t drawVertexCount = (iboMesh != nullptr) ? gpuMesh->indexCount : gpuMesh->vertexCount;

        passCtx.encoder.DrawInstanced<Shaders::Modules::MeshParticleRenderVS>(
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

void Draw3DParticleShadows(PassContext& passCtx) noexcept {
    auto& ctx = passCtx.ctx;
    if (!ctx.meshParticleShadowPipeline.Valid() || ctx.queues.MeshParticleEmitters().empty()) {
        return;
    }

    for (const auto& emitter: ctx.queues.MeshParticleEmitters()) {
        auto* pBuf    = ctx.geometry.Resolve(emitter.gpuBuffer);
        const Mesh*     gpuMesh = ctx.geometry.FindMesh(emitter.meshAsset);
        const Material* gpuMat  = ctx.geometry.FindMaterial(emitter.materialAsset);

        if ((pBuf == nullptr) || gpuMesh == nullptr || gpuMat == nullptr) {
            continue;
        }

        auto* posMesh     = ctx.geometry.Resolve(gpuMesh->posBuffer);
        auto* surfaceMesh = ctx.geometry.Resolve(gpuMesh->surfaceBuffer);
        auto* iboMesh     = (gpuMesh->indexBuffer != BufferHandle::Invalid) ? ctx.geometry.Resolve(gpuMesh->indexBuffer) : nullptr;
        if (posMesh == nullptr) {
            continue;
        }

        RenderContext::Impl::MeshParticleRenderPush rpc = {
            .particleBufferAddr  = ctx.BufferAddress(pBuf->buffer.Handle()),
            .posAddress          = posMesh->vboAddress,
            .baseColorFactor     = gpuMat->baseColorFactor,
            .emissiveFactor      = {},
            .tangentFrameAddress = 0,
            .surfaceAddress      = (surfaceMesh != nullptr) ? surfaceMesh->vboAddress : 0,
            .iboAddress          = (iboMesh != nullptr) ? iboMesh->vboAddress : 0,
            .indexCount          = gpuMesh->indexCount,
            .albedoIdx           = ctx.textureManager.GetBindlessIndex(gpuMat->albedoMap),
            .normalIdx           = 0,
            .pbrIdx              = 0,
            .emissiveIdx         = 0,
            .roughness           = 0.0f,
            .metallic            = 0.0f,
            .alphaCutoff         = gpuMat->alphaCutoff,
            .alphaMode           = gpuMat->alphaMode,
            .samplerCodes0       = PackMaterialSamplerAddresses(gpuMat->textureSamplers, 0),
            .samplerCodes1       = PackMaterialSamplerAddresses(gpuMat->textureSamplers, 8),
            .unlit               = 0u
        };

        uint32_t drawVertexCount = (iboMesh != nullptr) ? gpuMesh->indexCount : gpuMesh->vertexCount;

        passCtx.encoder.DrawInstanced<Shaders::Modules::MeshParticleShadowVS>(
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
        .anisotropy = Vk::Assume<Vk::ColorWrite<Res_Anisotropy>>(impl.graphResources.anisotropyBuffer),
        .sheen      = Vk::Assume<Vk::ColorWrite<Res_Sheen>>(impl.graphResources.sheenBuffer),
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

        if (IsForwardOnly(drawCmd.instanceData.flags) || (drawCmd.flags & DrawFlags::Viewmodel) != DrawFlags::None || drawMat->pipeline == VK_NULL_HANDLE) {
            currentPipeline = VK_NULL_HANDLE;
            continue;
        }

        if (i == 0 || drawMat->pipeline != currentPipeline) {
            groups.push_back(GroupRange {.material = drawMat, .start = i, .count = 1});
            currentPipeline = drawMat->pipeline;
        } else {
            groups.back().count++;
        }
    }

    return groups;
}

}
