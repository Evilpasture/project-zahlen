// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace ZHLN::AssetCooking {

// ZRD1 is an offline interchange format, understood by the cooker and
// optional asset-loading tools. Core only accepts prepared float pixels.
inline constexpr uint32_t kCookedRadianceMagic   = 0x3144525A;
inline constexpr uint32_t kCookedRadianceVersion = 1;

struct CookedRadianceHeader {
    uint32_t magic    = 0;
    uint32_t version  = 0;
    uint32_t width    = 0;
    uint32_t height   = 0;
    uint32_t dataSize = 0;
};
static_assert(sizeof(CookedRadianceHeader) == 20, "cooked radiance header gained padding; the file format is these five words");

} // namespace ZHLN::AssetCooking
