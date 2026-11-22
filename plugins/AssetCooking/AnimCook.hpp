// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// AssetCooking/AnimCook.hpp
//
// The animation baker: in-memory keyframe tracks in, `.zanim` bytes out.
// No manifest, no .bin files -- the caller (an importer, or the zcook CLI's
// Blender-IR bridge) supplies each track's times and packed values.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ZHLN::AssetCooking {

// Matches CookedAnimTrack::pathType in the cooked format.
inline constexpr uint32_t kAnimPathTranslation = 0;
inline constexpr uint32_t kAnimPathRotation    = 1;
inline constexpr uint32_t kAnimPathScale       = 2;

struct AnimTrackInput {
    uint64_t                   targetNodeHash = 0;
    uint32_t                   pathType       = kAnimPathTranslation;
    std::span<const float>     times;  // keyframe times, keyCount entries
    std::span<const std::byte> values; // packed keyframe values (vec3/vec4 floats)
};

struct AnimationInput {
    float                        duration = 0.0f;
    bool                         loop     = false;
    std::span<const AnimTrackInput> tracks;
};

[[nodiscard]] auto CookAnimation(const AnimationInput& input) -> std::vector<std::byte>;

} // namespace ZHLN::AssetCooking
