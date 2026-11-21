// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/Core/Description.hpp>
#include <cstdint>
namespace ZHLN::COM {
enum class FramePipelineError : uint8_t {
    FrameInProgress  ZHLN_ANNOTATION(ZHLN::Description<"A frame is already advancing or waiting for completion."> {}) = 1,
    NoPendingFrame   ZHLN_ANNOTATION(ZHLN::Description<"No advanced frame is waiting for completion."> {})            = 2,
    InvalidDeltaTime ZHLN_ANNOTATION(ZHLN::Description<"Frame delta must be finite and nonnegative."> {})             = 3,
};
} // namespace ZHLN::COM
