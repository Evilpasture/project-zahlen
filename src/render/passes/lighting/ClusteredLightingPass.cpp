// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/ClusteredLightingPass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void ClusteredLightingPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    const uint32_t fIdx = impl.presenter.frameIndex;

    // The pipeline has a ray-traced variant that shadows the rasterizer's
    // reflection term with a traced one; which specialization the graph runs
    // is this frame's setting, resolved here rather than being baked in.
    const bool   rtrActive    = impl.settings.rayTracing.enableReflections && impl.ctx.RayTracingSupported();
    const uint32_t lightVariant = rtrActive ? 1 : 0;

    const Vk::AsAddressWrite tlas {
        .address = (impl.ctx.RayTracingSupported() && impl.frames.tlas.Current()) ?
                       Vk::GetAccelerationStructureAddress(impl.ctx.Device(), impl.frames.tlas.Current().Get()) :
                       0
    };
    const Vk::HeapBlockBase block = impl.lightingPass.WriteHeapParameters<Shaders::Lighting>(
        impl.ctx, impl.heapManager,
        Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(impl.graphResources.sceneColor)),
        Vk::Slot<"texDepth">(Vk::Assume<Vk::ShaderRead<Res_Depth>>(impl.presenter.depthTarget)),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ShaderRead<Res_NormRough>>(impl.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"lights">(impl.frames.lightStorageBuffers[fIdx]),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"shadowMap">(Vk::Assume<Vk::ShaderRead<Res_ShadowMap>>(impl.graphResources.shadowMap)),
        Vk::Slot<"ltc_mat">(impl.ltcMatView),
        Vk::Slot<"ltc_amp">(impl.ltcAmpView),
        Vk::Slot<"clusterGrid">(impl.frames.clusterGridBuffers[fIdx]),
        Vk::Slot<"clusterIndexList">(impl.frames.lightIndexListBuffers[fIdx]),
        Vk::Slot<"punctualShadowCube">(impl.targets.AtlasCubeView()),
        Vk::Slot<"punctualShadow2D">(impl.targets.Atlas2DView()),
        Vk::Slot<"blueNoiseTex">(impl.textureManager.View(impl.blueNoiseTexIdx)),
        Vk::Slot<"texEmissive">(Vk::Assume<Vk::ShaderRead<Res_Emissive>>(impl.graphResources.emissiveBuffer)),
        Vk::Slot<"texAo">(Vk::Assume<Vk::ShaderRead<Res_Ao>>(impl.graphResources.ao)),
        Vk::Slot<"texClearcoat">(Vk::Assume<Vk::ShaderRead<Res_Clearcoat>>(impl.graphResources.clearcoatBuffer)),
        Vk::Slot<"tlas">(tlas)
    );
    impl.lightingPass.ExecuteVariantHeap<Shaders::Modules::LightingPS, Shaders::Modules::LightingNortPS>(impl.ctx, ctx.Cmd(), lightVariant, pc, block);
}

}
