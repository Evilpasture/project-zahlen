// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The error boundary between the Vulkan leaf module and the engine. src/vulkan
// speaks Vk::Error; everything above it speaks the engine's polymorphic
// ErrorCode. Crossing the boundary converts, and a device-lost VkResult is
// reported as FrameResult::DeviceLost, where the render layer expects it.

#include "Rendering.hpp"

namespace ZHLN::Vk {

// Boundary conversion: Vk::Error -> ErrorCode (the Vk::Error conversion applies
// the device-lost mapping).
[[nodiscard]] constexpr auto ToFrameError(const Vk::Error error) noexcept -> ErrorCode {
    return error;
}

[[nodiscard]] constexpr auto ToFrameError(const VkResult result) noexcept -> ErrorCode {
    return Vk::Error {result};
}

} // namespace ZHLN::Vk
