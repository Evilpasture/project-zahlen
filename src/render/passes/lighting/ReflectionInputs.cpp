// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/ReflectionInputs.hpp"

namespace ZHLN::Passes {

auto GatherReflectionInputs(RenderFrameContext& impl) noexcept -> ReflectionInputs {
    return ReflectionInputs {
        .prefiltered = Vk::ImageWrite {impl.iblPayload.prefilteredInfo},
        .visualSky   = Vk::ImageWrite {impl.iblPayload.visualSkyInfo.image != VK_NULL_HANDLE ? impl.iblPayload.visualSkyInfo :
                                      impl.textureManager.ViewInfo(kFallbackBlackTextureIndex)},
        .brdfLut     = Vk::ImageWrite {impl.iblPayload.brdfLutInfo},
        .blueNoise   = Vk::ImageWrite {impl.textureManager.ViewInfo(impl.blueNoiseTexIdx)},
        .tlas =
            Vk::AsAddressWrite {
                .address = (impl.ctx.RayTracingSupported() && impl.frames.tlasAddresses[impl.presenter.frameIndex] != nullptr) ?
                               impl.frames.tlasAddresses[impl.presenter.frameIndex].Get() :
                               0
            }
    };
}

auto ReflectionVariant(RenderFrameContext& impl) noexcept -> uint32_t {
    const bool rtrActive = impl.settings.rayTracing.enableReflections && impl.ctx.RayTracingSupported();
    return (impl.settings.post.enableSSR ? 1u : 0u) | (rtrActive ? 2u : 0u);
}

}
