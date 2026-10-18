// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pipelines/ReflectionPipeline.hpp"
#include "GBufferSurface.hpp"
#include "passes/lighting/ReflectionInputs.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN {

void ReflectionPipeline::Dispatch(
    Vk::RasterPassContextBase&           ctx,
    const Passes::GBufferSurface&        surface,
    const Slang::ScenePassPushConstants& pc
) const noexcept {
    RenderFrameContext&            impl   = _ctx;
    const uint32_t                 fIdx   = impl.presenter.frameIndex;
    const Passes::ReflectionInputs inputs = Passes::GatherReflectionInputs(impl);

    // The Set-1 scene mappings resolve through push-address sources: the frame
    // addresses must be in the push range before the draw (the decal pass does
    // the same), and the heap tables behind scene.materialSamplers /
    // scene.globalTextures bind here with them.
    impl.BindHeapsAndPushFrame(ctx.Cmd());

    const auto block = pass.WriteHeapParameters<Shaders::Reflection>(
        impl.heapManager, Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(impl.graphResources.sceneColor)), Vk::Slot<"texDepth">(surface.depth),
        Vk::Slot<"texNormalRoughness">(surface.normals), Vk::Slot<"texEnvMap">(inputs.prefiltered), Vk::Slot<"texSkyEquirect">(inputs.visualSky),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx]), Vk::Slot<"brdfLUT">(inputs.brdfLut),
        Vk::Slot<"texLighting">(Vk::Assume<Vk::ShaderRead<Res_LightingGI>>(impl.graphResources.lightingGI)),
        Vk::Slot<"texVoxelIntegrated">(Vk::Assume<Vk::ShaderReadGeneral<Res_VoxelResolved>>(impl.graphResources.voxelResolved)),
        Vk::Slot<"g_instances">(impl.frames.instanceDataBuffers[fIdx]), Vk::Slot<"blueNoiseTex">(inputs.blueNoise),
        Vk::Slot<"texRtrHalf">(Vk::Assume<Vk::ShaderRead<Res_RtrHalf>>(impl.graphResources.rtrHalf)),
        Vk::Slot<"texClearcoat">(Vk::Assume<Vk::ShaderRead<Res_Clearcoat>>(impl.graphResources.clearcoatBuffer)), Vk::Slot<"texSheen">(surface.sheen),
        Vk::Slot<"texAnisotropy">(surface.anisotropy), Vk::Slot<"tlas">(inputs.tlas),
        Vk::Slot<"texDdgiIrradiance">(Vk::Assume<Vk::ShaderRead<Res_DdgiIrradiance>>(impl.graphResources.ddgiIrradiance)),
        Vk::Slot<"texDdgiDistance">(Vk::Assume<Vk::ShaderRead<Res_DdgiDistance>>(impl.graphResources.ddgiDistance))
    );

    // Typestate: From proves the variant's bounds and validity, Create the
    // non-zero heap push -- ExecuteHeap cannot be reached with anything else.
    if (const auto active = Vk::ActivePipeline::From(pass.pipelines, Passes::ReflectionVariant(impl))) {
        if (const auto heap = Vk::BoundHeapPass::Create(pass.heapBindings.indexPushOffset, block.slot)) {
            pass.ExecuteHeap<Shaders::Modules::ReflectionPS, Shaders::Modules::ReflectionNortPS>(ctx, *active, *heap, pc);
        }
    }
}

} // namespace ZHLN
