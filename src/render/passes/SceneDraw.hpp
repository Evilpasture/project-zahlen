// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Core/Array.hpp>
#include <cstdint>

namespace ZHLN::Passes {

// Which of the two depth-only/main rasterization families a draw is being
// tested against. `DrawFlags` lets a draw opt into one or both; a draw that
// names neither is visible everywhere.
enum class RenderPassType : uint8_t { Main, Shadow };

[[nodiscard]] constexpr auto IsForwardOnly(uint32_t instanceFlags) noexcept -> bool {
    return (instanceFlags & 0xFF) == 2;
}

[[nodiscard]] auto IsVisibleIn(DrawFlags flags, RenderPassType passType) noexcept -> bool;

[[nodiscard]] auto UseMeshPath(const DrawCommand& drawCmd, VkPipeline pipelineOverride, bool meshShadingActive) noexcept -> bool;

[[nodiscard]] constexpr auto TaskGroupCount(uint32_t meshletCount) noexcept -> uint32_t {
    return (meshletCount + kMeshletsPerTaskGroup - 1) / kMeshletsPerTaskGroup;
}

// One draw, instanced from the CPU-side queue. The mesh path is taken when the
// draw's material has a mesh pipeline and its geometry was meshlet-built;
// otherwise this falls back to the indexed or non-indexed vertex draw.
template <typename T>
void SubmitDrawInstanced(
    Vk::CommandEncoder& encoder,
    const DrawCommand&  drawCmd,
    uint32_t            instanceIdx,
    const T&            pushConstants,
    bool                meshShadingActive,
    VkPipeline          pipelineOverride = VK_NULL_HANDLE,
    VkPipelineLayout    layoutOverride   = VK_NULL_HANDLE,
    VkShaderStageFlags  stages           = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
) noexcept {
    const auto* nativeMat = drawCmd.material;
    auto* const layout    = (layoutOverride != VK_NULL_HANDLE) ? layoutOverride : nativeMat->layout;

    if (UseMeshPath(drawCmd, pipelineOverride, meshShadingActive)) {
        encoder.DrawMeshTasks<Shaders::Modules::BasicTask>(
            {.pipeline    = nativeMat->meshPipeline.Get(),
             .layout      = layout,
             .heap        = true,
             .groupCountX = TaskGroupCount(drawCmd.instanceData.meshletCount),
             .groupCountY = 1,
             .groupCountZ = 1},
            pushConstants
        );
        return;
    }

    auto* pipeline = pipelineOverride;
    if (pipeline == VK_NULL_HANDLE && nativeMat != nullptr) {
        pipeline = nativeMat->pipeline.Get();
    }
    if (pipeline == VK_NULL_HANDLE) {
        return;
    }

    const uint32_t vertexCount = drawCmd.instanceData.iboAddress != 0 ? drawCmd.instanceData.indexCount : drawCmd.instanceData.vertexCount;

    encoder.DrawInstanced<Shaders::Modules::BasicVS, Shaders::Modules::BasicVSForward>(
        {.pipeline = pipeline, .layout = layout, .heap = true, .vertexCount = vertexCount, .instanceCount = 1, .firstVertex = 0, .firstInstance = instanceIdx},
        pushConstants, stages
    );
}

// Stencil-buffer constructive solid geometry: the cutters write the stencil,
// then the eye mesh is drawn against it to cut or intersect.
void DrawCSGMeshes(const FrameRecorder& recorder, VkExtent3D extent) noexcept;

void Draw3DParticles(const FrameRecorder& recorder) noexcept;

void Draw3DParticleShadows(const FrameRecorder& recorder) noexcept;

// The five GBuffer color targets plus the depth target, as the attachments a
// raster pass writes them as. Both GBuffer passes (and the viewmodel pass,
// which writes the same targets from a separate projection) resolve the set the
// same way, so the spelling lives here rather than in each pass.
using GBufferTargets = SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL>;

[[nodiscard]] auto GBufferSceneTargets(RenderContext::Impl& impl) noexcept -> GBufferTargets;

// Collapses the draw queue into runs of consecutive draws that share a
// pipeline, which is the granularity a GPU-culled indirect draw is issued at.
[[nodiscard]] auto BuildGroupRanges(const RenderContext::Impl& impl) -> ZHLN::Array<GroupRange>;

void StampScenePass(RenderContext::Impl::ScenePassStamp& stamp, const RenderContext::Impl& ctx, uint32_t drawCount, bool ran) noexcept;

}
