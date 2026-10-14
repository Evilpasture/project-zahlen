// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/ClusteredLightingPass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void ClusteredLightingPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    const uint32_t fIdx = frame.presenter.frameIndex;

    // The pipeline has a ray-traced variant that reads the denoised RT shadow
    // mask; which specialization the graph runs is this frame's setting,
    // resolved here rather than being baked in.
    const bool rtrActive =
        (frame.settings.rayTracing.enableReflections || frame.settings.rayTracing.enableShadows || frame.settings.rayTracing.enableDiffuseGI) &&
        frame.ctx.RayTracingSupported();
    const uint32_t lightVariant = rtrActive ? 1 : 0;

    const Vk::HeapBlockBase block = post.lighting.WriteHeapParameters<Shaders::Lighting>(
        frame.heapManager,
        Vk::Slot<"texInput">(Vk::Assume<Vk::FragmentAndComputeRead<Res_SceneColor>>(frame.graphResources.sceneColor)),
        Vk::Slot<"texDepth">(Vk::Assume<Vk::FragmentAndComputeRead<Res_Depth>>(frame.presenter.depthTarget)),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::FragmentAndComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"lights">(frame.frames.lightStorageBuffers[fIdx]),
        Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"shadowMap">(Vk::Assume<Vk::ShaderRead<Res_ShadowMap>>(frame.graphResources.shadowMap)),
        Vk::Slot<"ltc_mat">(post.ltcMat),
        Vk::Slot<"ltc_amp">(post.ltcAmp),
        Vk::Slot<"clusterGrid">(frame.frames.clusterGridBuffers[fIdx]),
        Vk::Slot<"clusterIndexList">(frame.frames.lightIndexListBuffers[fIdx]),
        Vk::Slot<"shadowAtlas">(frame.graphResources.shadowAtlas.image.ViewInfo(VK_IMAGE_ASPECT_DEPTH_BIT)),
        Vk::Slot<"shadowTiles">(frame.frames.shadowTileBuffers[fIdx]),
        Vk::Slot<"texEmissive">(Vk::Assume<Vk::ShaderRead<Res_Emissive>>(frame.graphResources.emissiveBuffer)),
        Vk::Slot<"texAo">(Vk::Assume<Vk::ShaderRead<Res_Ao>>(frame.graphResources.ao)),
        Vk::Slot<"texClearcoat">(Vk::Assume<Vk::FragmentAndComputeRead<Res_Clearcoat>>(frame.graphResources.clearcoatBuffer)),
        Vk::Slot<"texSheen">(Vk::Assume<Vk::FragmentAndComputeRead<Res_Sheen>>(frame.graphResources.sheenBuffer)),
        Vk::Slot<"texAnisotropy">(Vk::Assume<Vk::FragmentAndComputeRead<Res_Anisotropy>>(frame.graphResources.anisotropyBuffer)),
        Vk::Slot<"texRtShadowMask">(Vk::Assume<Vk::ShaderRead<Res_RtShadowMask>>(frame.graphResources.rtShadowMask)),
        Vk::Slot<"texDdgiIrradiance">(Vk::Assume<Vk::FragmentAndComputeRead<Res_DdgiIrradiance>>(frame.graphResources.ddgiIrradiance)),
        Vk::Slot<"texDdgiDistance">(Vk::Assume<Vk::FragmentAndComputeRead<Res_DdgiDistance>>(frame.graphResources.ddgiDistance))
    );
    post.lighting.ExecuteVariantHeap<Shaders::Modules::LightingPS, Shaders::Modules::LightingNortPS>(ctx.Cmd(), lightVariant, pc, block);
}

}
