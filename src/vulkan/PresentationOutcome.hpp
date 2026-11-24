// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/ErrorCode.hpp>
#include <cstdint>
#include <expected>
#include <optional>

namespace ZHLN::Vk {

template <typename T>
using PresentationOutcome = std::expected<std::optional<T>, ErrorCode>;

struct PresentSuboptimal {};

// There is no shared "device lost" code here on purpose. Device loss is a
// property of the six domains that can report it, each spelling the member
// itself (VulkanCallError, CommandPoolError, CommandRecordingError,
// CommandRingError, SwapchainError, PresentationError). Aggregating them into
// one question is the render layer's job: see IsDeviceLost in
// src/render/DeviceLoss.hpp.

} // namespace ZHLN::Vk
