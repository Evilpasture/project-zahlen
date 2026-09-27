// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/postprocess/BloomPass.hpp"
#include <ShaderBindings.hpp>
#include <algorithm>

namespace ZHLN::Passes {

// One Kawase tap. Mode 0 is the downsample, mode 1 the upsample that adds the
// next-coarser level; the reciprocal dimensions turn a thread id into a UV.
struct KawasePushConstants {
    int   mode;
    float rcpWidth;
    float rcpHeight;
    float glowIntensity;
};
static_assert(GpuAbi::ScenePassPayload<KawasePushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void BloomPass::operator()(VkCommandBuffer cmd) const noexcept {
    impl.BindHeapsAndPushFrame(cmd);

    const auto srcHdr     = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.hdrSceneColor);
    const auto emissive   = Vk::Assume<Vk::ComputeRead<Res_Emissive>>(impl.graphResources.emissiveBuffer);
    const auto thresh     = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.bloomThresholdTarget);
    const auto down1      = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.bloomDown1);
    const auto down2      = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.bloomDown2);
    const auto down3      = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.bloomDown3);
    const auto up2        = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.bloomUp2);
    const auto up1        = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.bloomUp1);
    const auto bloomFinal = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.bloomFinalTarget);

    Vk::ComputeChain thresholdChain(impl.ctx, impl.heapManager, cmd);
    Vk::ComputeChain downChain(impl.ctx, impl.heapManager, cmd);
    Vk::ComputeChain upChain(impl.ctx, impl.heapManager, cmd);

    const auto Kawase = [](int mode, const auto& src) noexcept {
        return KawasePushConstants {
            .mode          = mode,
            .rcpWidth      = 1.0f / static_cast<float>(src.extent.width),
            .rcpHeight     = 1.0f / static_cast<float>(src.extent.height),
            .glowIntensity = 0.0f
        };
    };

    auto thresholdPush          = Kawase(0, impl.graphResources.hdrSceneColor);
    thresholdPush.glowIntensity = std::max(impl.settings.post.glowIntensity, 0.0f);

    thresholdChain.Step<Shaders::BloomThreshold>(
        impl.postProcess.BloomThreshold(), impl.postProcess.BloomThresholdHeapBindings(), thresh.extent, thresholdPush,
        Vk::Slot<"texInput">(srcHdr),
        Vk::Slot<"texEmissive">(emissive),
        Vk::Slot<"outImage">(thresh)
    );

    Vk::MemoryBarrier(cmd, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderRead);

    downChain.Step<Shaders::BloomDown>(
        impl.postProcess.BloomDown(), impl.postProcess.BloomDownHeapBindings(), down1.extent, Kawase(0, thresh),
        Vk::Slot<"texInput">(thresh),
        Vk::Slot<"outImage">(down1)
    );
    downChain.Step<Shaders::BloomDown>(
        impl.postProcess.BloomDown(), impl.postProcess.BloomDownHeapBindings(), down2.extent, Kawase(0, down1),
        Vk::Slot<"texInput">(down1),
        Vk::Slot<"outImage">(down2)
    );
    downChain.Step<Shaders::BloomDown>(
        impl.postProcess.BloomDown(), impl.postProcess.BloomDownHeapBindings(), down3.extent, Kawase(0, down2),
        Vk::Slot<"texInput">(down2),
        Vk::Slot<"outImage">(down3)
    );

    Vk::MemoryBarrier(cmd, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderRead);

    upChain.Step<Shaders::BloomUp>(
        impl.postProcess.BloomUp(), impl.postProcess.BloomUpHeapBindings(), up2.extent, Kawase(1, down3),
        Vk::Slot<"texInput">(down3),
        Vk::Slot<"texLow">(down2),
        Vk::Slot<"outImage">(up2)
    );
    upChain.Step<Shaders::BloomUp>(
        impl.postProcess.BloomUp(), impl.postProcess.BloomUpHeapBindings(), up1.extent, Kawase(1, up2),
        Vk::Slot<"texInput">(up2),
        Vk::Slot<"texLow">(down1),
        Vk::Slot<"outImage">(up1)
    );
    upChain.Step<Shaders::BloomUp>(
        impl.postProcess.BloomUp(), impl.postProcess.BloomUpHeapBindings(), bloomFinal.extent, Kawase(1, up1),
        Vk::Slot<"texInput">(up1),
        Vk::Slot<"texLow">(thresh),
        Vk::Slot<"outImage">(bloomFinal)
    );
}

}
