// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

namespace ZHLN { struct CapturedFrame; }

// The renderer owns readback. This extension owns encoding and file I/O.
namespace ZHLN::Screenshot {
// Write display-referred RGB as PNG, dropping alpha; returns false on an
// invalid frame, unsupported dimensions, encoding failure, or I/O failure.
[[nodiscard]] auto WritePNG(std::string_view path, const CapturedFrame& frame) -> bool;
} // namespace ZHLN::Screenshot
