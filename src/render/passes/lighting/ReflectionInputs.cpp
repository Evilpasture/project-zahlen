// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/ReflectionInputs.hpp"
#include "Renderer.hpp"

namespace ZHLN::Passes {

auto GatherReflectionInputs(RenderFrame& frame) noexcept -> ReflectionInputs {
    return ReflectionInputs {
        .prefiltered = Vk::ImageWrite {frame.renderer.iblPayload.prefilteredView},
        .visualSky =
            Vk::ImageWrite {
                frame.renderer.iblPayload.visualSkyView.Handle() != VK_NULL_HANDLE ? frame.renderer.iblPayload.visualSkyView :
                                                                                     frame.renderer.textureManager.View(kFallbackBlackTextureIndex)
            },
        .brdfLut = Vk::ImageWrite {frame.renderer.iblPayload.brdfLutView},
        .tlas    = Vk::AsAddressWrite {.address = (frame.renderer.ctx.RayTracingSupported() && frame.tlasAddresses != nullptr) ? frame.tlasAddresses.Get() : 0}
    };
}

auto ReflectionVariant(RenderFrame& frame) noexcept -> uint32_t {
    const bool rtrActive = frame.settings.rayTracing.enableReflections && frame.renderer.ctx.RayTracingSupported();
    return (frame.settings.post.enableSSR ? 1u : 0u) | (rtrActive ? 2u : 0u);
}

} // namespace ZHLN::Passes
