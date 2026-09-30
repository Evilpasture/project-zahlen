// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace ZHLN::AssetCooking {

// ZRD1 is accepted for backward compatibility; ZRD2 contains separate
// original/conditioned panoramas and the extracted sun. Core only receives
// prepared float pixels and metadata, never these container headers.
inline constexpr uint32_t kCookedRadianceMagic   = 0x3144525A;
inline constexpr uint32_t kCookedRadianceVersion = 1;
inline constexpr uint32_t kPreparedRadianceMagic   = 0x3244525A; // "ZRD2"
inline constexpr uint32_t kPreparedRadianceVersion = 2;

struct CookedRadianceHeader {
    uint32_t magic    = 0;
    uint32_t version  = 0;
    uint32_t width    = 0;
    uint32_t height   = 0;
    uint32_t dataSize = 0;
};
static_assert(sizeof(CookedRadianceHeader) == 20, "cooked radiance header gained padding; the file format is these five words");

// Fixed-size metadata precedes visual RGBA32F, then optionally conditioned
// RGBA32F. All sizes are byte counts; no native pointers, std::optional, or
// renderer types enter the file. `hasSun` implies a complete conditioned map
// and valid direction, RGB irradiance/pi, and 9x3 smooth irradiance SH.
struct PreparedRadianceHeader {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t visualDataSize = 0;
    uint32_t lightingDataSize = 0;
    uint32_t hasSun = 0;
    float    sunDirection[3] {};
    float    sunIrradiance[3] {};
    float    diffuseSH[9][3] {};
};
static_assert(sizeof(PreparedRadianceHeader) == 160, "ZRD2 header layout changed");

} // namespace ZHLN::AssetCooking
