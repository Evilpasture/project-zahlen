// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <cstdint>
#include <vector>

namespace ZHLN::COM {
struct IEntities;
}

namespace ZHLN::ALife {

struct ALifeComponent;

class ZHLN_STATIC_API SpatialGrid {
  public:
    SpatialGrid(uint32_t w, uint32_t h, float cell_size);

    void               Clear() noexcept;
    void               UpdateEntity(COM::IEntities& entities, Entity handle, DVec3 old_pos);
    void               RemoveEntity(COM::IEntities& entities, Entity handle);
    [[nodiscard]] auto Query(const COM::IEntities& entities, DVec3 pos, float radius, std::vector<Entity> reusable = {}) const -> std::vector<Entity>;
    [[nodiscard]] auto GetCellIndex(DVec3 pos) const noexcept -> int32_t;

    std::vector<Entity> _cellHeads;
    uint32_t            _width;
    uint32_t            _height;
    float               _cellSize;
};

} // namespace ZHLN::ALife
