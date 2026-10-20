// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/ReflectionInputs.hpp"

namespace ZHLN::Passes {

auto GatherReflectionInputs(RenderFrameContext& frame) noexcept -> ReflectionInputs {
    return ReflectionInputs {
        .prefiltered = Vk::ImageWrite {frame.iblPayload.prefilteredView},
        .visualSky =
            Vk::ImageWrite {
                frame.iblPayload.visualSkyView.Handle() != VK_NULL_HANDLE ? frame.iblPayload.visualSkyView :
                                                                             frame.textureManager.View(kFallbackBlackTextureIndex)
            },
        .brdfLut   = Vk::ImageWrite {frame.iblPayload.brdfLutView},
        .blueNoise = Vk::ImageWrite {frame.textureManager.View(frame.blueNoiseTexIdx)},
        .tlas      = Vk::AsAddressWrite {
                 .address = (frame.ctx.RayTracingSupported() && frame.frames.tlasAddresses[frame.presenter.frameIndex] != nullptr) ?
                                frame.frames.tlasAddresses[frame.presenter.frameIndex].Get() :
                                0
        }
    };
}

auto ReflectionVariant(RenderFrameContext& frame) noexcept -> uint32_t {
    const bool rtrActive = frame.settings.rayTracing.enableReflections && frame.ctx.RayTracingSupported();
    return (frame.settings.post.enableSSR ? 1u : 0u) | (rtrActive ? 2u : 0u);
}

} // namespace ZHLN::Passes
