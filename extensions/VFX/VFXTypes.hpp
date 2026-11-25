// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Math/Vec3.hpp>
#include <cstdint>
#include <type_traits>

namespace ZHLN::Explosion {

enum class Kind : uint8_t { StandardFireball, ArtilleryMortar };

struct SpawnInfo {
    FVec3 origin {};
    float scale = 1;
    Kind  kind  = Kind::ArtilleryMortar;
};

static_assert(std::is_standard_layout_v<SpawnInfo> && std::is_trivially_copyable_v<SpawnInfo>);

} // namespace ZHLN::Explosion

namespace ZHLN::Lightning {

enum class Phase : uint8_t { SteppedLeader, ReturnStroke, Dissipating };

struct ConfigInfo {
    float eta = 2, peakCurrentKA = 45, timeDilation = 1, ribbonWidth = .8f;
    int   subdivisions = 5;
    float soundVolume = 10, emissiveIntensity = 8000;
};

static_assert(std::is_standard_layout_v<ConfigInfo> && std::is_trivially_copyable_v<ConfigInfo>);

} // namespace ZHLN::Lightning
