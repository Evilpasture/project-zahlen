// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Render/GpuEnums.hpp
#pragma once
#include <cstdint>

namespace ZHLN {

// The engine enums two GPU structs keep as fields (Light.type,
// ParticleEmitterParams.alignment: uint in Slang, these spellings on the
// host). They stay outside <Zahlen/Render/GpuLayout.hpp> because the generated
// header includes them back -- it spells the field `LightType` -- so it cannot
// be named from here.

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
