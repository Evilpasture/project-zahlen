// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Error.hpp>
#include <cstdint>
#include <expected>

namespace ZHLN::Vk {

/// One WSI surface plus the per-frame graphics objects that present it.
/// Primary and extra Engine windows both own one of these.
struct SwapchainSession {
    Surface                                  surface;
    PresentationContext                      presentation;
    FrameSync<2>                             sync;
    CommandPools<2, QueueType::Graphics>     pools;
    uint32_t                                 frameIndex = 0;

    [[nodiscard]] auto Init(
        const Context& ctx, Allocator& alloc, uint32_t width, uint32_t height, uint32_t graphicsFamily, bool vsync = true
    ) -> std::expected<void, Error> {
        if (auto r = presentation.Init(ctx, alloc, surface.Get(), width, height, vsync); !r) {
            return r;
        }
        sync  = FrameSync<2>::Create(ctx.Device());
        pools = CommandPools<2, QueueType::Graphics>::Create(ctx.Device(), {.queueFamily = graphicsFamily, .buffersPerPool = 1});
        frameIndex = 0;
        if (!sync.Valid() || !pools.Valid()) {
            return std::unexpected(PresentationError::SyncCreationFailed);
        }
        return {};
    }

    [[nodiscard]] auto DrawDesc(const Context& ctx) const noexcept -> DrawFrameDesc<2> {
        return {
            .ctx               = ctx,
            .swapchain         = presentation.swapchain,
            .sync              = sync,
            .pools             = pools,
            .presentSemaphores = presentation.presentSemaphores,
        };
    }
};

} // namespace ZHLN::Vk
