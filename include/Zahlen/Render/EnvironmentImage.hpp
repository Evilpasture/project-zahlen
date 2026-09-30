// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <vector>

namespace ZHLN {

// Owned, top-down, linear RGBA float pixels. The engine consumes this data;
// loading and decoding source images belong to the application or cooker.
struct EnvironmentImage {
    uint32_t           width       = 0;
    uint32_t           height      = 0;
    std::vector<float> rgba;
    // Optional. When zero, RenderContext hashes the pixels before baking IBL.
    uint64_t           contentHash = 0;
};

} // namespace ZHLN
