// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/ClusteredLightingPass.hpp"
#include "RenderFrame.hpp"
#include "Renderer.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void ClusteredLightingPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    const bool               hasTlas      = frame.renderer.ctx.RayTracingSupported() && frame.tlasAddresses != nullptr;
    const auto lightVariant = ((frame.settings.rayTracing.enableReflections || frame.settings.rayTracing.enableShadows) && hasTlas) ? 1 : 0;
    const Vk::AsAddressWrite tlas {.address = hasTlas ? frame.tlasAddresses.Get() : 0};

    const auto block = post.lighting.WriteHeapParameters<Shaders::Lighting>(
        frame.renderer.heapManager, Vk::Slot<"texInput">(Vk::Assume<Vk::FragmentAndComputeRead<Res_SceneColor>>(frame.graphResources.sceneColor)),
        Vk::Slot<"texDepth">(Vk::Assume<Vk::FragmentAndComputeRead<Res_Depth>>(frame.ActiveDepthTarget())),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::FragmentAndComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"texBakedMask">(Vk::Assume<Vk::FragmentAndComputeRead<Res_BakedMask>>(frame.graphResources.bakedMask)),
        Vk::Slot<"lights">(frame.lightStorageBuffers), Vk::Slot<"frame">(frame.frameUniformBuffers),
        Vk::Slot<"shadowMap">(Vk::Assume<Vk::ShaderRead<Res_ShadowMap>>(frame.graphResources.shadowMap)), Vk::Slot<"ltc_mat">(post.ltcMat),
        Vk::Slot<"ltc_amp">(post.ltcAmp), Vk::Slot<"clusterGrid">(frame.clusterGridBuffers), Vk::Slot<"clusterIndexList">(frame.lightIndexListBuffers),
        Vk::Slot<"shadowAtlas">(frame.graphResources.shadowAtlas.image.View(VK_IMAGE_ASPECT_DEPTH_BIT)), Vk::Slot<"shadowTiles">(frame.shadowTileBuffers),
        Vk::Slot<"texAo">(Vk::Assume<Vk::ShaderRead<Res_Ao>>(frame.graphResources.ao)), Vk::Slot<"tlas">(tlas)
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
