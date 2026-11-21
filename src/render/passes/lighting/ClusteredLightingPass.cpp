// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/passes/lighting/ClusteredLightingPass.hpp"
#include "render/RenderFrame.hpp"
#include "render/Renderer.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void ClusteredLightingPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    // No ray is traced here any more: the sun's PCSS and contact ray are the
    // SunShadowMaskPass dispatch, and this pass reads its result. Only the
    // pipeline variant still depends on the device, because lighting.slang is
    // compiled twice -- once with rayQuery types available, once without -- and a
    // device that cannot run the first must be given the second.
    const auto lightVariant = frame.renderer.ctx.RayTracingSupported() ? 1 : 0;

    const auto block = post.lighting.WriteHeapParameters<Shaders::Lighting>(
        frame.renderer.heapManager, Vk::Slot<"texInput">(Vk::Assume<Vk::FragmentAndComputeRead<Res_SceneColor>>(frame.graphResources.sceneColor)),
        Vk::Slot<"texDepth">(Vk::Assume<Vk::FragmentAndComputeRead<Res_Depth>>(frame.ActiveDepthTarget())),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::FragmentAndComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"texBakedMask">(Vk::Assume<Vk::FragmentAndComputeRead<Res_BakedMask>>(frame.graphResources.bakedMask)),
        Vk::Slot<"lights">(frame.lightStorageBuffers), Vk::Slot<"frame">(frame.frameUniformBuffers),
        Vk::Slot<"texSunShadowMask">(Vk::Assume<Vk::FragmentAndComputeRead<Res_SunShadowMask>>(frame.graphResources.sunShadowMask)),
        Vk::Slot<"ltc_mat">(post.ltcMat),
        Vk::Slot<"ltc_amp">(post.ltcAmp), Vk::Slot<"clusterGrid">(frame.clusterGridBuffers), Vk::Slot<"clusterIndexList">(frame.lightIndexListBuffers),
        Vk::Slot<"shadowAtlas">(frame.graphResources.shadowAtlas.image.View(VK_IMAGE_ASPECT_DEPTH_BIT)), Vk::Slot<"shadowTiles">(frame.shadowTileBuffers),
        Vk::Slot<"texAo">(Vk::Assume<Vk::ShaderRead<Res_Ao>>(frame.graphResources.ao))
    );
    // Typestate: From proves the variant's bounds and validity, Create the
    // non-zero heap push -- ExecuteHeap cannot be reached with anything else.
    if (const auto active = Vk::ActivePipeline::From(post.lighting.pipelines, lightVariant)) {
        if (const auto heap = Vk::BoundHeapPass::Create(post.lighting.heapBindings.indexPushOffset, block.slot)) {
            post.lighting.ExecuteHeap<Shaders::Modules::LightingPS, Shaders::Modules::LightingNortPS>(ctx, *active, *heap, pc);
        }
    }
}

} // namespace ZHLN::Passes
