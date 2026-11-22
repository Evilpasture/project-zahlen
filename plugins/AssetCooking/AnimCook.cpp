// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <AssetCooking/AnimCook.hpp>
#include <Zahlen/Render/CookedFormats.hpp>
#include <cstring>

namespace ZHLN::AssetCooking {

auto CookAnimation(const AnimationInput& input) -> std::vector<std::byte> {
    std::vector<CookedAnimTrack> tracks;
    std::vector<uint8_t>         payloadData;

    auto appendPayload = [&payloadData](std::span<const std::byte> bytes) -> uint32_t {
        while (payloadData.size() % 4 != 0) {
            payloadData.push_back(0);
        }
        auto localOffset = static_cast<uint32_t>(payloadData.size());
        const auto* first = reinterpret_cast<const uint8_t*>(bytes.data());
        payloadData.insert(payloadData.end(), first, first + bytes.size());
        return localOffset;
    };

    for (const auto& track: input.tracks) {
        CookedAnimTrack cooked {};
        cooked.targetNodeHash = track.targetNodeHash;
        cooked.pathType       = track.pathType;
        cooked.keyCount       = static_cast<uint32_t>(track.times.size());
        cooked.timeOffset     = appendPayload(std::as_bytes(track.times));
        cooked.valueOffset    = appendPayload(track.values);
        tracks.push_back(cooked);
    }

    CookedAnimHeader header {};
    header.duration   = input.duration;
    header.loop       = input.loop ? 1 : 0;
    header.trackCount = static_cast<uint32_t>(tracks.size());

    std::vector<std::byte> out;
    const auto append = [&out](const void* data, size_t size) {
        const auto* first = static_cast<const std::byte*>(data);
        out.insert(out.end(), first, first + size);
    };
    append(&header, sizeof(CookedAnimHeader));
    if (!tracks.empty()) append(tracks.data(), tracks.size() * sizeof(CookedAnimTrack));
    if (!payloadData.empty()) append(payloadData.data(), payloadData.size());
    return out;
}

} // namespace ZHLN::AssetCooking
