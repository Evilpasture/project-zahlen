// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/ReflectionInputs.hpp"

namespace ZHLN::Passes {

auto GatherReflectionInputs(RenderContext::Impl& impl) noexcept -> ReflectionInputs {
    return ReflectionInputs {
        .prefiltered =
            Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                .handle   = impl.iblPayload.prefilteredImage.Handle(),
                .view     = impl.iblPayload.prefilteredView.Get(),
                .extent   = {.width = 128, .height = 128, .depth = 1},
                .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                .format   = impl.iblPayload.prefilteredFormat,
                .viewInfo = &impl.iblPayload.prefilteredViewInfo
            },
        .brdfLut =
            Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                .handle   = impl.iblPayload.brdfLutImage.Handle(),
                .view     = impl.iblPayload.brdfLutView.Get(),
                .extent   = {.width = 512, .height = 512, .depth = 1},
                .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                .format   = VK_FORMAT_R8G8B8A8_UNORM,
                .viewInfo = &impl.iblPayload.brdfLutViewInfo
            },
        .blueNoise =
            Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                .handle   = impl.textureManager.Image(impl.blueNoiseTexIdx).Handle(),
                .view     = impl.textureManager.View(impl.blueNoiseTexIdx).Get(),
                .extent   = {.width = impl.blueNoiseWidth, .height = impl.blueNoiseHeight, .depth = 1},
                .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                .format   = VK_FORMAT_R8G8B8A8_UNORM,
                .viewInfo = &impl.blueNoiseViewInfo
            },
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
