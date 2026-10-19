// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/postprocess/BloomPass.hpp"
#include "PassPayloads.hpp"
#include <ShaderBindings.hpp>
#include <algorithm>

namespace ZHLN::Passes {

void BloomPass::operator()(VkCommandBuffer cmd) const noexcept {
    using enum Vk::BarrierStage;
    using enum Vk::BarrierAccess;
    using namespace Shaders;
    frame.BindHeapsAndPushFrame(cmd);

    const auto srcHdr     = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.hdrSceneColor);
    const auto thresh     = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.bloomThresholdTarget);
    const auto down1      = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.bloomDown1);
    const auto down2      = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.bloomDown2);
    const auto down3      = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.bloomDown3);
    const auto up2        = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.bloomUp2);
    const auto up1        = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.bloomUp1);
    const auto bloomFinal = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.bloomFinalTarget);

    Vk::ComputeChain thresholdChain(frame.heapManager, cmd);
    Vk::ComputeChain downChain(frame.heapManager, cmd);
    Vk::ComputeChain upChain(frame.heapManager, cmd);

    const auto Kawase = [](int mode, const auto& src) noexcept {
        return KawasePushConstants {
            .mode          = mode,
            .rcpWidth      = 1.0f / static_cast<float>(src.Extent().width),
            .rcpHeight     = 1.0f / static_cast<float>(src.Extent().height),
            .glowIntensity = 0.0f
        };
    };

    ThresholdPushConstants thresholdPush {
        .mode          = 0,
        .rcpWidth      = 1.0f / static_cast<float>(srcHdr.Extent().width),
        .rcpHeight     = 1.0f / static_cast<float>(srcHdr.Extent().height),
        .glowIntensity = std::max(frame.settings.post.glowIntensity, 0.0f),
        .exposure      = std::max(frame.settings.post.exposure, 1e-4f)
    };

    thresholdChain.Step<BloomThreshold>(
        frame.postProcess.BloomThreshold(), frame.postProcess.BloomThresholdHeapBindings(), thresh.Extent(), thresholdPush, Vk::Slot<"texInput">(srcHdr),
        Vk::Slot<"outImage">(thresh)
    );

    Vk::MemoryBarrier(cmd, Compute, ShaderWrite, Compute, ShaderRead);

    downChain.Step<BloomDown>(
        frame.postProcess.BloomDown(), frame.postProcess.BloomDownHeapBindings(), down1.Extent(), Kawase(0, thresh), Vk::Slot<"texInput">(thresh),
        Vk::Slot<"outImage">(down1)
    );
    downChain.Step<BloomDown>(
        frame.postProcess.BloomDown(), frame.postProcess.BloomDownHeapBindings(), down2.Extent(), Kawase(0, down1), Vk::Slot<"texInput">(down1),
        Vk::Slot<"outImage">(down2)
    );
    downChain.Step<BloomDown>(
        frame.postProcess.BloomDown(), frame.postProcess.BloomDownHeapBindings(), down3.Extent(), Kawase(0, down2), Vk::Slot<"texInput">(down2),
        Vk::Slot<"outImage">(down3)
    );

    Vk::MemoryBarrier(cmd, Compute, ShaderWrite, Compute, ShaderRead);

    upChain.Step<BloomUp>(
        frame.postProcess.BloomUp(), frame.postProcess.BloomUpHeapBindings(), up2.Extent(), Kawase(1, down3), Vk::Slot<"texInput">(down3),
        Vk::Slot<"texLow">(down2), Vk::Slot<"outImage">(up2)
    );
    upChain.Step<BloomUp>(
        frame.postProcess.BloomUp(), frame.postProcess.BloomUpHeapBindings(), up1.Extent(), Kawase(1, up2), Vk::Slot<"texInput">(up2),
        Vk::Slot<"texLow">(down1), Vk::Slot<"outImage">(up1)
    );
    upChain.Step<BloomUp>(
        frame.postProcess.BloomUp(), frame.postProcess.BloomUpHeapBindings(), bloomFinal.Extent(), Kawase(1, up1), Vk::Slot<"texInput">(up1),
        Vk::Slot<"texLow">(thresh), Vk::Slot<"outImage">(bloomFinal)
    );
}

} // namespace ZHLN::Passes
