// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RadianceEncoder.hpp"
#include "CookedRadianceFormat.hpp"
#include "EnvironmentPreparation.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace ZHLN::AssetCooking {

auto EncodeCookedRadiance(const EnvironmentImage& image) -> std::vector<std::byte> {
    // The public writer is also usable without DecodeRadiance; condition the
    // supplied raw pixels here if they have not already been prepared. zcook's
    // decoded images take the idempotent, already-prepared path.
    if (image.width == 0 || image.height == 0 || image.width > 8192 || image.height > 8192) return {};
    const size_t pixels = static_cast<size_t>(image.width) * image.height * 4u;
    if (image.rgba.size() != pixels || (!image.lightingRgba.empty() && image.lightingRgba.size() != pixels) ||
        (image.sun && image.lightingRgba.empty())) return {};
    EnvironmentImage prepared = image;
    PrepareEnvironmentImage(prepared);
    const size_t visualBytes = pixels * sizeof(float);
    const size_t lightingBytes = prepared.lightingRgba.empty() ? 0 : visualBytes;
    constexpr size_t kMaxFileBytes = 512u * 1024u * 1024u;
    if (prepared.width == 0 || prepared.height == 0 || prepared.rgba.size() != pixels ||
        (!prepared.lightingRgba.empty() && prepared.lightingRgba.size() != pixels) ||
        (prepared.sun && lightingBytes == 0) ||
        visualBytes > std::numeric_limits<uint32_t>::max() ||
        visualBytes > kMaxFileBytes - sizeof(PreparedRadianceHeader) ||
        lightingBytes > kMaxFileBytes - sizeof(PreparedRadianceHeader) - visualBytes) return {};
    if (prepared.sun) {
        float lengthSq = 0.0f;
        for (size_t c = 0; c < 3; ++c) {
            const float dir = prepared.sun->direction[c], energy = prepared.sun->irradiance[c];
            if (!std::isfinite(dir) || !std::isfinite(energy) || energy < 0.0f) return {};
            lengthSq += dir * dir;
        }
        if (!std::isfinite(lengthSq) || std::abs(lengthSq - 1.0f) > 0.01f) return {};
        for (const auto& coeff: prepared.sun->diffuseSH)
            for (float channel: coeff)
                if (!std::isfinite(channel)) return {};
    }

    PreparedRadianceHeader header {
        .magic = kPreparedRadianceMagic,
        .version = kPreparedRadianceVersion,
        .width = prepared.width,
        .height = prepared.height,
        .visualDataSize = static_cast<uint32_t>(visualBytes),
        .lightingDataSize = static_cast<uint32_t>(lightingBytes),
        .hasSun = prepared.sun ? 1u : 0u,
    };
    if (prepared.sun) {
        std::memcpy(header.sunDirection, prepared.sun->direction.data(), sizeof(header.sunDirection));
        std::memcpy(header.sunIrradiance, prepared.sun->irradiance.data(), sizeof(header.sunIrradiance));
        for (size_t c = 0; c < 9; ++c) {
            std::memcpy(header.diffuseSH[c], prepared.sun->diffuseSH[c].data(), sizeof(header.diffuseSH[c]));
        }
    }
    std::vector<std::byte> out(sizeof(header) + visualBytes + lightingBytes);
    std::memcpy(out.data(), &header, sizeof(header));
    std::memcpy(out.data() + sizeof(header), prepared.rgba.data(), visualBytes);
    if (lightingBytes != 0) {
        std::memcpy(out.data() + sizeof(header) + visualBytes, prepared.lightingRgba.data(), lightingBytes);
    }
    return out;
}

} // namespace ZHLN::AssetCooking
