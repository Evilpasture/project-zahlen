// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/Render/CapturedFrame.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <expected>
#include <string_view>

namespace ZHLN::COM { struct IGraphicsDevice; }

// The renderer owns readback. This extension owns encoding and file I/O.
namespace ZHLN::Screenshot {
// Reusable caller-owned storage for COM readback. Read once with a conservative
// extent * 8 buffer, rather than querying through a second GPU readback. A
// larger capture retries once with the reported byte count. No GPU cache.
// Calls must be serialized; a successful pointer lives until the next Read or
// destruction. Neither this object nor its returned frame owns the device.
class CaptureBuffer {
public:
    [[nodiscard]] auto Read(COM::IGraphicsDevice& graphics, Extent2D extent)
        -> std::expected<const CapturedFrame*, ErrorCode>;
private:
    CapturedFrame _frame;
};

// Write display-referred RGB as PNG, dropping alpha; returns false on an
// invalid frame, unsupported dimensions, encoding failure, or I/O failure.
[[nodiscard]] auto WritePNG(std::string_view path, const CapturedFrame& frame) -> bool;
} // namespace ZHLN::Screenshot
