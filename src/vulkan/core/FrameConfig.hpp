// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>

namespace ZHLN::Vk {

// Number of CPU/GPU frame slots (not the number of swapchain images or parallel recording workers).
inline constexpr uint32_t kFramesInFlight = 2;
static_assert(kFramesInFlight >= 2 && kFramesInFlight <= 8, "the render frame ring needs at least two slots; frame sync supports at most eight");

[[nodiscard]] constexpr auto FrameSlot(uint32_t frameIndex) noexcept -> uint32_t {
    return frameIndex % kFramesInFlight;
}

[[nodiscard]] constexpr auto NextFrameSlot(uint32_t frameIndex) noexcept -> uint32_t {
    return (FrameSlot(frameIndex) + 1) % kFramesInFlight;
}

[[nodiscard]] constexpr auto PreviousFrameSlot(uint32_t frameIndex) noexcept -> uint32_t {
    return (FrameSlot(frameIndex) + kFramesInFlight - 1) % kFramesInFlight;
}

}
