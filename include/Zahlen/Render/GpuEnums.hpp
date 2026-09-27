// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>

namespace ZHLN {


enum class LightType : uint32_t {
    Directional,
    Point,
    Spot,
    Area,
    Sun,
};
static_assert(sizeof(LightType) == sizeof(uint32_t));

enum class ParticleAlignment : uint32_t { CameraBillboard = 0, VelocityStretched = 1, GroundFlat = 2 };

}
