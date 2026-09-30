// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/ReflectionCompositePass.hpp"
#include "passes/lighting/ReflectionInputs.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void ReflectionCompositePass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    const uint32_t fIdx = impl.presenter.frameIndex;

    const ReflectionInputs inputs = GatherReflectionInputs(impl);

    const Vk::HeapBlockBase block = impl.reflectionPass.WriteHeapParameters<Shaders::Reflection>(
        impl.ctx, impl.heapManager,
        Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(impl.graphResources.sceneColor)),
        Vk::Slot<"texDepth">(Vk::Assume<Vk::ShaderRead<Res_Depth>>(impl.presenter.depthTarget)),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ShaderRead<Res_NormRough>>(impl.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"texEnvMap">(inputs.prefiltered),
        Vk::Slot<"texSkyEquirect">(inputs.visualSky),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"brdfLUT">(inputs.brdfLut),
        Vk::Slot<"texLighting">(Vk::Assume<Vk::ShaderRead<Res_Lighting>>(impl.graphResources.lightingTarget)),
        Vk::Slot<"texVoxelIntegrated">(Vk::Assume<Vk::ShaderReadGeneral<Res_VoxelResolved>>(impl.graphResources.voxelResolved)),
        Vk::Slot<"g_instances">(impl.frames.instanceDataBuffers[fIdx]),
        Vk::Slot<"blueNoiseTex">(inputs.blueNoise),
        Vk::Slot<"texRtrHalf">(Vk::Assume<Vk::ShaderRead<Res_RtrHalf>>(impl.graphResources.rtrHalf)),
        Vk::Slot<"texClearcoat">(Vk::Assume<Vk::ShaderRead<Res_Clearcoat>>(impl.graphResources.clearcoatBuffer)),
        Vk::Slot<"texSheen">(Vk::Assume<Vk::ShaderRead<Res_Sheen>>(impl.graphResources.sheenBuffer)),
        Vk::Slot<"texAnisotropy">(Vk::Assume<Vk::ShaderRead<Res_Anisotropy>>(impl.graphResources.anisotropyBuffer)),
        Vk::Slot<"tlas">(inputs.tlas)
    );

    impl.reflectionPass.ExecuteVariantHeap<Shaders::Modules::ReflectionPS, Shaders::Modules::ReflectionNortPS>(
        impl.ctx, ctx.Cmd(), ReflectionVariant(impl), pc, block
    );
}

}
