// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RadianceEncoder.hpp"
#include "CookedRadianceFormat.hpp"
#include <cstring>

namespace ZHLN::AssetCooking {

auto EncodeCookedRadiance(const EnvironmentImage& image) -> std::vector<std::byte> {
    const size_t pixels  = static_cast<size_t>(image.width) * image.height * 4u;
    const size_t payload = pixels * sizeof(float);
    CookedRadianceHeader header {
        .magic    = kCookedRadianceMagic,
        .version  = kCookedRadianceVersion,
        .width    = image.width,
        .height   = image.height,
        .dataSize = static_cast<uint32_t>(payload),
    };
    std::vector<std::byte> out(sizeof(header) + payload);
    std::memcpy(out.data(), &header, sizeof(header));
    if (payload > 0 && image.rgba.size() >= pixels) {
        std::memcpy(out.data() + sizeof(header), image.rgba.data(), payload);
    }
    return out;
}

} // namespace ZHLN::AssetCooking
