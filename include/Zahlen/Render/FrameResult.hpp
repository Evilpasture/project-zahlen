// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <cstdint>
#include <expected>
#include <optional>

namespace ZHLN {

template <typename T>
using FrameOutcome = std::expected<std::optional<T>, ErrorCode>;

struct FrameSkipped {};

struct PresentSuboptimal {};

enum class FrameResult : uint8_t {
    TargetRecreationFailed ZHLN_ANNOTATION(ZHLN::Description<"The renderer could not recreate the swapchain and its targets"> {}) = 1,

    DeviceLost ZHLN_ANNOTATION(ZHLN::Description<"The graphics device was lost (VK_ERROR_DEVICE_LOST)"> {}),
};

static_assert(static_cast<uint32_t>(FrameResult::TargetRecreationFailed) != 0, "ErrorCode's 0 value means 'no error'; no FrameResult may use it.");

}
