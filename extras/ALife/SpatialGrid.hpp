// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Entity.hpp>
#include <cstdint>
#include <vector>

// Forward declare engine registry
namespace ZHLN::ECS {
class Registry;
}

namespace ZHLN::ALife {

class ZHLN_API SpatialGrid {
  public:
    SpatialGrid(uint32_t w, uint32_t h, float cell_size);

    void Clear() noexcept;
    void UpdateEntity(ECS::Registry& reg, Entity handle, JPH::RVec3Arg old_pos);
    void RemoveEntity(ECS::Registry& reg, Entity handle);

    // Returns number of items found and populates out_buffer
    auto Query(const ECS::Registry& reg, JPH::RVec3Arg pos, float radius, std::vector<Entity>& out_buffer) const -> uint32_t;

    [[nodiscard]] auto GetCellIndex(JPH::RVec3Arg pos) const noexcept -> int32_t;

    std::vector<uint32_t> _cellHeads;
    uint32_t              _width;
    uint32_t              _height;
    float                 _cellSize;
};

} // namespace ZHLN::ALife
