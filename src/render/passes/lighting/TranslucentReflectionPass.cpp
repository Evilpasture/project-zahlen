// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/TranslucentReflectionPass.hpp"
#include "passes/lighting/ReflectionInputs.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void TranslucentReflectionPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    const uint32_t fIdx = impl.presenter.frameIndex;

    const ReflectionInputs inputs = GatherReflectionInputs(impl);

    const Vk::HeapBlockBase block = impl.translucentReflectionPass.WriteHeapParameters<Shaders::Reflection>(
        impl.ctx, impl.heapManager,
        Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(impl.graphResources.sceneColor)),
        Vk::Slot<"texDepth">(Vk::Assume<Vk::ShaderRead<Res_TransDepth>>(impl.graphResources.transDepthBuffer)),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ShaderRead<Res_TransNorm>>(impl.graphResources.transNormalBuffer)),
        Vk::Slot<"texEnvMap">(inputs.prefiltered),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"brdfLUT">(inputs.brdfLut),
        Vk::Slot<"texLighting">(Vk::Assume<Vk::ShaderRead<Res_Lighting>>(impl.graphResources.lightingTarget)),
        Vk::Slot<"texVoxelIntegrated">(Vk::Assume<Vk::ShaderReadGeneral<Res_VoxelResolved>>(impl.graphResources.voxelResolved)),
        Vk::Slot<"g_instances">(impl.frames.instanceDataBuffers[fIdx]),
        Vk::Slot<"blueNoiseTex">(inputs.blueNoise),
        Vk::Slot<"texRtrHalf">(Vk::Assume<Vk::ShaderRead<Res_RtrHalf>>(impl.graphResources.rtrHalf)),
        Vk::Slot<"texClearcoat">(Vk::Assume<Vk::ShaderRead<Res_Clearcoat>>(impl.graphResources.clearcoatBuffer)),
        Vk::Slot<"texAnisotropy">(Vk::Assume<Vk::ShaderRead<Res_TransAnisotropy>>(impl.graphResources.transAnisotropyBuffer)),
        Vk::Slot<"tlas">(inputs.tlas)
    );

    impl.translucentReflectionPass.ExecuteVariantHeap<Shaders::Modules::ReflectionPS, Shaders::Modules::ReflectionNortPS>(
        impl.ctx, ctx.Cmd(), ReflectionVariant(impl), pc, block
    );
}

}
