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

// Lift a leaf-module expected across the boundary: same value type, with the
// Vk::Error converted to ErrorCode (applying the device-lost mapping).
// and_then/or_else require the lambda's error type to match the source
// expected's error type exactly, so engine-side chains wrap their
// Vk::Error-typed sources (or Vk::Error-typed lambda results) with this.
template <typename T>
[[nodiscard]] constexpr auto ToEngineExpected(std::expected<T, Vk::Error> result) noexcept -> std::expected<T, ErrorCode> {
    return std::expected<T, ErrorCode> {std::move(result)};
}

} // namespace ZHLN::Vk
