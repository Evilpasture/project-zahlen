// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Meshlet.hpp>
#include <cstdint>

namespace ZHLN::Passes {

enum class RenderPassType : uint8_t { Main, Shadow };

[[nodiscard]] constexpr auto IsForwardOnly(uint32_t instanceFlags) noexcept -> bool {
    return (instanceFlags & 0xFFu) == 2u || (instanceFlags & (1u << 10)) != 0u;
}

[[nodiscard]] auto IsVisibleIn(DrawFlags flags, RenderPassType passType) noexcept -> bool;

[[nodiscard]] auto UseMeshPath(const DrawCommand& drawCmd, VkPipeline pipelineOverride, bool meshShadingActive) noexcept -> bool;

[[nodiscard]] constexpr auto TaskGroupCount(uint32_t meshletCount) noexcept -> uint32_t {
    return (meshletCount + kMeshletsPerTaskGroup - 1) / kMeshletsPerTaskGroup;
}

template <typename T>
void SubmitDrawInstanced(
    Vk::CommandEncoder& encoder,
    const DrawCommand&  drawCmd,
    const uint32_t      instanceIdx,
    const T&            pushConstants,
    const bool          meshShadingActive,
    VkPipeline          pipelineOverride = VK_NULL_HANDLE
) noexcept {
    const auto* nativeMat   = drawCmd.material;
    const auto  doubleSided = (drawCmd.instanceData.flags & (1U << 9U)) != 0;
    encoder.SetCullMode(doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);

    if (UseMeshPath(drawCmd, pipelineOverride, meshShadingActive)) {
        encoder.DrawMeshTasks<Shaders::Modules::BasicTask>(
            {.pipeline = nativeMat->meshPipeline, .groupCountX = TaskGroupCount(drawCmd.instanceData.meshletCount), .groupCountY = 1, .groupCountZ = 1},
            pushConstants
        );
        return;
    }

    // An override is the pass's own pipeline, not a reason to skip the draw:
    // BackgroundGBufferPass hands over the material's pipeline, DrawCSGMeshes
    // hands over the stencil/difference/intersection pipelines, and
    // TranslucentPrePass hands over the prepass pipeline. Only a draw that has
    // neither an override nor a material pipeline is undrawable. This branch
    // used to be an if/else over the *material* test, so every caller that
    // supplied an override returned here and submitted nothing -- which is why
    // the 3D skybox never rasterized.
    auto* pipeline = pipelineOverride;
    if (pipeline == VK_NULL_HANDLE) {
        if (nativeMat == nullptr || nativeMat->pipeline == VK_NULL_HANDLE) {
            return;
        }
        pipeline = nativeMat->pipeline;
    }

    const auto vertexCount = drawCmd.instanceData.iboAddress != 0 ? drawCmd.instanceData.indexCount : drawCmd.instanceData.vertexCount;

    encoder.DrawInstanced<Shaders::Modules::BasicVS, Shaders::Modules::BasicVSForward>(
        {.pipeline = pipeline, .vertexCount = vertexCount, .instanceCount = 1, .firstVertex = 0, .firstInstance = instanceIdx}, pushConstants
    );
}

void DrawCSGMeshes(PassContext& passCtx, const CsgResources& csg, VkExtent3D extent) noexcept;

void Draw3DParticles(PassContext& passCtx, const MeshParticlePipelines& particles) noexcept;

void Draw3DParticleShadows(PassContext& passCtx, const MeshParticlePipelines& particles) noexcept;

using GBufferTargets = SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL>;

[[nodiscard]] auto GBufferSceneTargets(RenderFrameContext& impl) noexcept -> GBufferTargets;

[[nodiscard]] auto BuildGroupRanges(const RenderFrameContext& frame) -> ZHLN::Array<GroupRange>;

} // namespace ZHLN::Passes
