// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>

namespace ZHLN {

// Explicit diagnostics sink: the caller owns the counters, the renderer
// writes to them. Passed through RenderConfig; never ambient state.
struct DiagnosticsSink {
    std::atomic<uint32_t>* validation = nullptr;
    std::atomic<uint32_t>* deviceLost = nullptr;

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return validation != nullptr && deviceLost != nullptr;
    }
};

} // namespace ZHLN
