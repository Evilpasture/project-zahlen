// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "GBufferSurface.hpp"
#include "passes/lighting/ReflectionInputs.hpp"
#include "pipelines/ReflectionPipeline.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN {

void ReflectionPipeline::Dispatch(
    Vk::RasterPassContextBase& ctx,
    const Passes::GBufferSurface& surface,
    const GeneratedGpu::ScenePassPushConstants& pc
) const noexcept {
    RenderContext::Impl& impl = _impl;
    const uint32_t fIdx = impl.presenter.frameIndex;
    const Passes::ReflectionInputs inputs = Passes::GatherReflectionInputs(impl);

    const Vk::HeapBlockBase block = pass.WriteHeapParameters<Shaders::Reflection>(
        impl.ctx, impl.heapManager,
        Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(impl.graphResources.sceneColor)),
        Vk::Slot<"texDepth">(surface.depth),
        Vk::Slot<"texNormalRoughness">(surface.normals),
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
        Vk::Slot<"texSheen">(surface.sheen),
        Vk::Slot<"texAnisotropy">(surface.anisotropy),
        Vk::Slot<"tlas">(inputs.tlas)
    );

    pass.ExecuteVariantHeap<Shaders::Modules::ReflectionPS, Shaders::Modules::ReflectionNortPS>(
        impl.ctx, ctx.Cmd(), Passes::ReflectionVariant(impl), pc, block
    );
}

}
