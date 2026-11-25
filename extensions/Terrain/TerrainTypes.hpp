// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "TerrainComponents.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <cstdint>
#include <type_traits>

namespace ZHLN::Terrain {

enum class Error : uint8_t {
    InvalidData ZHLN_ANNOTATION(ZHLN::Description<"Invalid heightfield dimensions, samples, colors or pose."> {}) = 1,
    NotStarted  ZHLN_ANNOTATION(ZHLN::Description<"Terrain host has no engine binding."> {})
};

struct GenerateInfo {
    uint32_t sampleCount = 128;
    float    worldSize   = 280;
    float    maxHeight   = 35;
    Type     type        = Type::Default;
};

struct CreateInfo {
    FVec3 position {};
    float roughness     = 0.85f;
    float metallic      = 0.05f;
    bool  createPhysics = true;
    bool  buildBLAS     = true;
};

static_assert(std::is_standard_layout_v<GenerateInfo> && std::is_trivially_copyable_v<GenerateInfo>);
static_assert(std::is_standard_layout_v<CreateInfo> && std::is_trivially_copyable_v<CreateInfo>);

} // namespace ZHLN::Terrain
