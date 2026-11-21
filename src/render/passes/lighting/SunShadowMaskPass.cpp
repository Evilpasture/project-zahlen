// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/passes/lighting/SunShadowMaskPass.hpp"
#include "render/RenderFrame.hpp"
#include "render/Renderer.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

using SunShadowPush = Slang::SunShadowPushConstants;
static_assert(GpuAbi::ScenePassPayload<SunShadowPush>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void SunShadowMaskPass::operator()(VkCommandBuffer cmd) const noexcept {
    // Nothing below is worth a dispatch when the sun cannot reach the frame: a
    // full-bright frame, or a sun that contributes no intensity this frame. The
    // mask then keeps whatever the graph last wrote and lighting.slang never
    // reads it, because its own `hasSunContribution` gate folds
    // `unshadowedSun * 0` to zero before the fetch.
    // Full bright is the environment's own switch, and it is authoritative here:
    // a renderer source may repoint the sun's direction without repacking the
    // frame uniforms, so the intensity lane is a hint, not a verdict.
    if (frame.settings.environment.fullBright != 0) {
        return;
    }

    const auto extent = frame.graphResources.sunShadowMask.extent;
    if (extent.width == 0u || extent.height == 0u) {
        return;
    }

    auto& lighting = frame.renderer.lighting;
    if (!lighting.sunShadowMask.Valid()) {
        return;
    }

    // The TLAS descriptor gets the acceleration structure's address whenever one
    // was built, not only on the frames that may trace: a descriptor write of a
    // zero address is a validation error (VUID-VkResourceDescriptorInfoEXT-type-
    // 11483), and the mask pass' heap block is written every frame it runs.
    // Whether the ray may fire is the shader's `frame.enableRTR` gate -- the one
    // answer RenderGraphDetail::AssembleScenePushConstants produces and both the
    // push block and FrameUniforms carry -- and that is where the reflection and
    // shadow switches are read.
    const Vk::AsAddressWrite tlas {.address = frame.tlasAddresses != nullptr ? frame.tlasAddresses.Get() : 0};

    const SunShadowPush push = {
        // Cascade PCSS reconstructs the world position from the same matrix the
        // G-buffer was rasterized with, so the surface, its normal and this
        // thread's depth refer to one point in space. The frozen, unjittered
        // projection is that matrix -- the jittered one would displace every
        // mask texel half a pixel away from the depth it belongs to.
        .invViewProj  = frame.uniforms.invViewProj,
        // The contact ray's bias policy, moved out of the shader so the trace and
        // its range read as one authored decision: a base 5 mm sink along the
        // normal, 0.25 mm per metre of view depth, and 4 mm more at grazing
        // incidence where a fixed bias cannot sit clear of the surface.
        .contactBias  = FVec4 {0.005f, 0.00025f, 0.004f, 0.0f},
        // Full occlusion out to 1.2 m, handoff complete at 3.0 m -- the range the
        // in-fragment ray used -- then the contact term retires across the far
        // fade that the lighting pass used to apply itself.
        .contactRange = FVec4 {1.2f, 3.0f, 80.0f, 100.0f},
    };

    const auto block = frame.renderer.heapManager.WriteHeapParameters<Shaders::SunShadowMask>(
        lighting.sunShadowMask.bindings,
        Vk::Slot<"texDepth">(Vk::Assume<Vk::FragmentAndComputeRead<Res_Depth>>(frame.ActiveDepthTarget())),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::FragmentAndComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"frame">(frame.frameUniformBuffers),
        Vk::Slot<"shadowMap">(Vk::Assume<Vk::ShaderRead<Res_ShadowMap>>(frame.graphResources.shadowMap)),
        Vk::Slot<"outSunShadowMask">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.sunShadowMask)), Vk::Slot<"tlas">(tlas)
    );

    // R8_UNORM is a required storage-image format: writing the mask needs no
    // optional feature the device might not have, which is why the mask is R8
    // and not R16.
    lighting.sunShadowMask.pass.DispatchHeapIndexedThreads<Shaders::Modules::SunShadowMaskCS>(
        cmd, block, extent.width, extent.height, 1u, push
    );
}

} // namespace ZHLN::Passes
