// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/ReflectionInputs.hpp"

namespace ZHLN::Passes {

auto GatherReflectionInputs(RenderContext::Impl& impl) noexcept -> ReflectionInputs {
    return ReflectionInputs {
        .prefiltered = Vk::ImageWrite {impl.iblPayload.prefilteredView},
        .brdfLut     = Vk::ImageWrite {impl.iblPayload.brdfLutView},
        .blueNoise   = Vk::ImageWrite {impl.textureManager.View(impl.blueNoiseTexIdx)},
        .tlas =
            Vk::AsAddressWrite {
                .address = (impl.ctx.RayTracingSupported() && impl.frames.tlas.Current()) ?
                               Vk::GetAccelerationStructureAddress(impl.ctx.Device(), impl.frames.tlas.Current().Get()) :
                               0
            }
    };
}

auto ReflectionVariant(RenderContext::Impl& impl) noexcept -> uint32_t {
    const bool rtrActive = impl.settings.rayTracing.enableReflections && impl.ctx.RayTracingSupported();
    return (impl.settings.post.enableSSR ? 1u : 0u) | (rtrActive ? 2u : 0u);
}

}
