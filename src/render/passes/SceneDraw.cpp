// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/SceneDraw.hpp"
#include "Renderer.hpp"
#include <Zahlen/Profiler.hpp>

namespace ZHLN::Passes {

auto IsVisibleIn(DrawFlags flags, RenderPassType passType) noexcept -> bool {
    using enum DrawFlags;
    // Skybox scenery rasterizes in its own G-buffer pass --
    // BackgroundGBufferPass -- which runs straight after the base pass and
    // shares its depth range, so the map wins every pixel it covers as an
    // ordinary depth test. It is primary visibility only: it never casts into
    // the shadow passes (a 300 m backdrop would shade the whole level).
    if ((flags & Background) != None) {
        return passType == RenderPassType::Main;
    }
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

void DrawCSGMeshes(RenderFrame& ctx, Vk::CommandEncoder& encoder, const CsgResources& csg, VkExtent3D extent) noexcept {
    auto* const cmd = encoder.cmd;

    auto* const stencilWritePipeline = csg.write.Get();
    if (ctx.renderer.queues.CsgDraws().empty() || stencilWritePipeline == VK_NULL_HANDLE) {
        return;
    }

    const ZHLN::ScopedTimer profTimer("GPU Stencil CSG Passes");

    for (const auto& csgCmd: ctx.renderer.queues.CsgDraws()) {
        Vk::ClearStencilAttachment(cmd, {.width = extent.width, .height = extent.height});

        for (const auto& cutter: csgCmd.cutters) {
            SubmitDrawInstanced(
                encoder, cutter.draw, cutter.instanceIdx, ObjectConstants {.instanceId = cutter.instanceIdx, .isShadowPass = 0}, ctx.meshShading,
                stencilWritePipeline
            );
        }

        auto* activePipeline = csg.difference.Get();
        if (!csgCmd.cutters.empty() && csgCmd.cutters[0].operation == CSGOperation::Intersection) {
            activePipeline = csg.intersection.Get();
        }
        if (activePipeline == VK_NULL_HANDLE) {
            return;
        }

        SubmitDrawInstanced(
            encoder, csgCmd.eyeDraw, csgCmd.eyeInstanceIdx, ObjectConstants {.instanceId = csgCmd.eyeInstanceIdx, .isShadowPass = 0}, ctx.meshShading,
            activePipeline
        );
    }
}

void Draw3DParticles(RenderFrame& ctx, Vk::CommandEncoder& encoder, const MeshParticlePipelines& particles) noexcept {
    if (!particles.render.Valid() || ctx.renderer.queues.MeshParticleEmitters().empty()) {
        return;
    }

    for (const auto& emitter: ctx.renderer.queues.MeshParticleEmitters()) {
        const auto* const pBuf    = ctx.renderer.geometry.Resolve(emitter.gpuBuffer);
        const auto        gpuMesh = ctx.renderer.geometry.FindMesh(emitter.meshAsset);
        const auto        gpuMat  = ctx.renderer.geometry.FindMaterial(emitter.materialAsset);

        if ((pBuf == nullptr) || !gpuMesh || !gpuMat) {
            continue;
        }

        const auto* const posMesh     = ctx.renderer.geometry.Resolve(gpuMesh->posBuffer);
        const auto* const frameMesh   = ctx.renderer.geometry.Resolve(gpuMesh->tangentFrameBuffer);
        const auto* const surfaceMesh = ctx.renderer.geometry.Resolve(gpuMesh->surfaceBuffer);
        const auto* const iboMesh     = (gpuMesh->indexBuffer != BufferHandle::Invalid) ? ctx.renderer.geometry.Resolve(gpuMesh->indexBuffer) : nullptr;
        if (posMesh == nullptr || frameMesh == nullptr || surfaceMesh == nullptr || iboMesh == nullptr) {
            continue;
        }

        auto drawVertexCount = (iboMesh != nullptr) ? gpuMesh->indexCount : gpuMesh->vertexCount;

        encoder.DrawInstanced<Shaders::Modules::MeshParticleRenderVS>(
            {.pipeline = particles.render.Get(), .vertexCount = drawVertexCount, .instanceCount = emitter.maxParticles, .firstVertex = 0, .firstInstance = 0},
            MeshParticleRenderPush {
                .particleBufferAddr  = ctx.renderer.BufferAddress(pBuf->buffer.Handle()),
                .posAddress          = posMesh->vboAddress.Get(),
                .baseColorFactor     = {gpuMat->baseColorFactor[0], gpuMat->baseColorFactor[1], gpuMat->baseColorFactor[2], gpuMat->baseColorFactor[3]},
                .emissiveFactor      = {gpuMat->emissiveFactor[0], gpuMat->emissiveFactor[1], gpuMat->emissiveFactor[2], gpuMat->emissiveFactor[3]},
                .tangentFrameAddress = (frameMesh != nullptr) ? frameMesh->vboAddress.Get() : 0,
                .surfaceAddress      = (surfaceMesh != nullptr) ? surfaceMesh->vboAddress.Get() : 0,
                .iboAddress          = (iboMesh != nullptr) ? iboMesh->vboAddress.Get() : 0,
                .indexCount          = gpuMesh->indexCount,
                .albedoIdx           = ctx.renderer.textureManager.GetBindlessIndex(gpuMat->albedoMap),
                .normalIdx           = ctx.renderer.textureManager.GetBindlessIndex(gpuMat->normalMap),
                .pbrIdx              = ctx.renderer.textureManager.GetBindlessIndex(gpuMat->pbrMap),
                .emissiveIdx         = ctx.renderer.textureManager.GetBindlessIndex(gpuMat->emissiveMap),
                .roughness           = gpuMat->roughnessFactor,
                .metallic            = gpuMat->metallicFactor,
                .alphaCutoff         = gpuMat->alphaCutoff,
                .alphaMode           = gpuMat->alphaMode,
                .samplerCodes0       = PackMaterialSamplerAddresses(gpuMat->textureSamplers, 0),
                .samplerCodes1       = PackMaterialSamplerAddresses(gpuMat->textureSamplers, 8),
                .unlit               = gpuMat->unlit ? 1u : 0u
            }
        );
    }
}

void Draw3DParticleShadows(RenderFrame& ctx, Vk::CommandEncoder& encoder, const MeshParticlePipelines& particles) noexcept {
    if (!particles.shadow.Valid() || ctx.renderer.queues.MeshParticleEmitters().empty()) {
        return;
    }

    for (const auto& emitter: ctx.renderer.queues.MeshParticleEmitters()) {
        const auto* const pBuf    = ctx.renderer.geometry.Resolve(emitter.gpuBuffer);
        const auto        gpuMesh = ctx.renderer.geometry.FindMesh(emitter.meshAsset);
        const auto        gpuMat  = ctx.renderer.geometry.FindMaterial(emitter.materialAsset);

        if ((pBuf == nullptr) || !gpuMesh || !gpuMat) {
            continue;
        }

        const auto* const posMesh     = ctx.renderer.geometry.Resolve(gpuMesh->posBuffer);
        const auto* const surfaceMesh = ctx.renderer.geometry.Resolve(gpuMesh->surfaceBuffer);
        const auto* const iboMesh     = (gpuMesh->indexBuffer != BufferHandle::Invalid) ? ctx.renderer.geometry.Resolve(gpuMesh->indexBuffer) : nullptr;
        if (posMesh == nullptr || surfaceMesh == nullptr || iboMesh == nullptr) {
            continue;
        }

        const MeshParticleRenderPush rpc = {
            .particleBufferAddr  = ctx.renderer.BufferAddress(pBuf->buffer.Handle()),
            .posAddress          = posMesh->vboAddress.Get(),
            .baseColorFactor     = JPH::Float4 {gpuMat->baseColorFactor[0], gpuMat->baseColorFactor[1], gpuMat->baseColorFactor[2], gpuMat->baseColorFactor[3]},
            .emissiveFactor      = {},
            .tangentFrameAddress = 0,
            .surfaceAddress      = (surfaceMesh != nullptr) ? surfaceMesh->vboAddress.Get() : 0,
            .iboAddress          = (iboMesh != nullptr) ? iboMesh->vboAddress.Get() : 0,
            .indexCount          = gpuMesh->indexCount,
            .albedoIdx           = ctx.renderer.textureManager.GetBindlessIndex(gpuMat->albedoMap),
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

        encoder.DrawInstanced<Shaders::Modules::MeshParticleShadowVS>(
            {.pipeline = particles.shadow.Get(), .vertexCount = drawVertexCount, .instanceCount = emitter.maxParticles, .firstVertex = 0, .firstInstance = 0},
            rpc
        );
    }
}

auto GBufferSceneTargets(RenderFrame& frame) noexcept -> GBufferTargets {
    return GBufferTargets {
        .sceneColor = Vk::Assume<Vk::ColorWrite<Res_SceneColor>>(frame.graphResources.sceneColor),
        .velocity   = Vk::Assume<Vk::ColorWrite<Res_Velocity>>(frame.graphResources.velocityBuffer),
        .normRough  = Vk::Assume<Vk::ColorWrite<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer),
        .bakedMask  = Vk::Assume<Vk::ColorWrite<Res_BakedMask>>(frame.graphResources.bakedMask),
        .depth      = Vk::Assume<Vk::DepthStencilWrite<Res_Depth>>(frame.ActiveDepthTarget())
    };
}

auto BuildGroupRanges(const RenderFrame& frame) -> ZHLN::Array<GroupRange> {
    using enum DrawFlags;
    const auto              drawCount = static_cast<uint32_t>(frame.renderer.queues.Draws().size());
    ZHLN::Array<GroupRange> groups;
    groups.reserve((drawCount + 15) / 16);

    VkPipeline currentPipeline = VK_NULL_HANDLE;

    for (uint32_t i = 0; i < drawCount; ++i) {
        const auto&       drawCmd = frame.renderer.queues.Draws()[i];
        const auto* const drawMat = drawCmd.material;

        if (IsForwardOnly(drawCmd.instanceData.flags) || (drawCmd.flags & Viewmodel) != None || (drawCmd.flags & Background) != None ||
            drawMat->pipeline == VK_NULL_HANDLE) {
            currentPipeline = VK_NULL_HANDLE;
            continue;
        }

        const VkCullModeFlags cullMode = (drawCmd.instanceData.flags & (1U << 9U)) != 0 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        if (groups.empty() || drawMat->pipeline != currentPipeline || groups.back().cullMode != cullMode) {
            groups.push_back({.material = drawMat, .start = i, .count = 1, .cullMode = cullMode});
            currentPipeline = drawMat->pipeline;
        } else {
            groups.back().count++;
        }
    }

    return groups;
}

} // namespace ZHLN::Passes
