// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <cstdint>
#include <expected>
#include <optional>

namespace ZHLN::Vk {

// Tri-state presentation result: value, benign skip (empty optional), or ErrorCode.
template <typename T>
using PresentationOutcome = std::expected<std::optional<T>, ErrorCode>;

struct PresentSuboptimal {};

enum class DeviceFault : uint8_t {
    Lost ZHLN_ANNOTATION(ZHLN::Description<"The graphics device was lost (VK_ERROR_DEVICE_LOST)"> {}) = 1,
};

[[nodiscard]] constexpr auto IsDeviceLost(ErrorCode code) noexcept -> bool {
    return code.Is(DeviceFault::Lost);
}

} // namespace ZHLN::Vk
