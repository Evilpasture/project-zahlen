// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pipelines/ReflectionPipeline.hpp"
#include "GBufferSurface.hpp"
#include "RenderFrame.hpp"
#include "Renderer.hpp"
#include "passes/lighting/ReflectionInputs.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN {

void ReflectionPipeline::Dispatch(
    RenderFrame&                         frame,
    Vk::RasterPassContextBase&           ctx,
    const Passes::GBufferSurface&        surface,
    const Slang::ScenePassPushConstants& pc
) const noexcept {
    const Passes::ReflectionInputs inputs = Passes::GatherReflectionInputs(frame);

    // The Set-1 scene mappings resolve through push-address sources, and the
    // heap tables behind scene.materialSamplers / scene.globalTextures: both are
    // state of the command stream this draw records into, established once when
    // that stream began (RenderContext::Impl::AcquireTarget), never per draw.

    const auto block = pass.WriteHeapParameters<Shaders::Reflection>(
        frame.renderer.heapManager, Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(frame.graphResources.sceneColor)),
        Vk::Slot<"texDepth">(surface.depth), Vk::Slot<"texNormalRoughness">(surface.normals),
        Vk::Slot<"texHiZ">(Vk::Assume<Vk::ShaderRead<Res_HiZ>>(frame.graphResources.hizMap)), Vk::Slot<"texEnvMap">(inputs.prefiltered),
        Vk::Slot<"texSkyEquirect">(inputs.visualSky), Vk::Slot<"frame">(frame.frameUniformBuffers), Vk::Slot<"brdfLUT">(inputs.brdfLut),
        Vk::Slot<"texLighting">(Vk::Assume<Vk::ShaderRead<Res_LightingGI>>(frame.graphResources.lightingGI)),
        Vk::Slot<"texLightingAo">(Vk::Assume<Vk::ShaderRead<Res_LightingAo>>(frame.graphResources.lightingAo)),
        Vk::Slot<"texVoxelIntegrated">(Vk::Assume<Vk::ShaderReadGeneral<Res_VoxelResolved>>(frame.graphResources.voxelResolved)),
        Vk::Slot<"g_instances">(frame.instanceDataBuffers), Vk::Slot<"tlas">(inputs.tlas),
        Vk::Slot<"texRcgiProbes">(Vk::Assume<Vk::ShaderRead<Res_RcgiProbes>>(frame.graphResources.rcgiProbes)),
        Vk::Slot<"texRcgiCascade0">(Vk::Assume<Vk::ShaderRead<Res_RcgiCascadeA>>(frame.graphResources.rcgiCascadeA))
    );

    // Typestate: From proves the variant's bounds and validity, Create the
    // non-zero heap push -- ExecuteHeap cannot be reached with anything else.
    if (const auto active = Vk::ActivePipeline::From(pass.pipelines, Passes::ReflectionVariant(frame))) {
        if (const auto heap = Vk::BoundHeapPass::Create(pass.heapBindings.indexPushOffset, block.slot)) {
            pass.ExecuteHeap<Shaders::Modules::ReflectionPS, Shaders::Modules::ReflectionNortPS>(ctx, *active, *heap, pc);
        }
    }
}

} // namespace ZHLN
