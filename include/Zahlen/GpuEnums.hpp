// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/GpuEnums.hpp
#pragma once
#include <cstdint>

namespace ZHLN {

// The engine enums two GPU structs keep as fields (Light.type,
// ParticleEmitterParams.alignment: uint in Slang, these spellings on the
// host). Split out of Types.hpp so the generated GPU structs -- which Types.hpp
// includes -- can name them without including Types.hpp back: the two headers
// would otherwise form a cycle. Types.hpp re-exports these.

enum class LightType : uint32_t {
    Directional,
    Point,
    Spot,
    Area,
    Sun,
};
static_assert(sizeof(LightType) == sizeof(uint32_t));

enum class ParticleAlignment : uint32_t { CameraBillboard = 0, VelocityStretched = 1, GroundFlat = 2 };

} // namespace ZHLN
