// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpatialGrid.hpp"
#include "ALifeComponents.hpp"
#include <Zahlen/COM/IEntities.hpp>
#include <algorithm>
#include <cmath>

namespace ZHLN::ALife {

SpatialGrid::SpatialGrid(uint32_t w, uint32_t h, float cell_size): _width(w), _height(h), _cellSize(cell_size) {
    _cellHeads.resize(static_cast<size_t>(w) * h, Entity::Null());
}

void SpatialGrid::Clear() noexcept {
    std::ranges::fill(_cellHeads, Entity::Null());
}

auto SpatialGrid::GetCellIndex(DVec3 pos) const noexcept -> int32_t {
    if (pos.x < 0.0 || pos.z < 0.0) return -1;
    const auto cell_size = static_cast<double>(_cellSize);
    const auto gx        = static_cast<int32_t>(pos.x / cell_size);
    const auto gz        = static_cast<int32_t>(pos.z / cell_size);
    if (gx >= static_cast<int32_t>(_width) || gz >= static_cast<int32_t>(_height)) return -1;
    return (gz * static_cast<int32_t>(_width)) + gx;
}

void SpatialGrid::UpdateEntity(COM::IEntities& entities, Entity handle, DVec3 old_pos) {
    auto* comp = entities.Get<ALifeComponent>(handle);
    if (comp == nullptr) return;
    if (comp->self_entity == Entity::Null()) comp->self_entity = handle;

    const int32_t old_idx = GetCellIndex(old_pos);
    const int32_t new_idx = GetCellIndex(comp->position);
    if (old_idx == new_idx) return;

    if (old_idx != -1) {
        Entity* curr = &_cellHeads[old_idx];
        while (*curr != Entity::Null()) {
            if (*curr == handle) {
                *curr = comp->next_in_grid;
                break;
            }
            auto* curr_comp = entities.Get<ALifeComponent>(*curr);
            if (curr_comp == nullptr) break;
            curr = &curr_comp->next_in_grid;
        }
    }

    comp->next_in_grid = Entity::Null();
    if (new_idx != -1) {
        comp->next_in_grid  = _cellHeads[new_idx];
        _cellHeads[new_idx] = handle;
    }
}

void SpatialGrid::RemoveEntity(COM::IEntities& entities, Entity handle) {
    auto* comp = entities.Get<ALifeComponent>(handle);
    if (comp == nullptr) return;

    const int32_t idx = GetCellIndex(comp->position);
    if (idx != -1) {
        Entity* curr = &_cellHeads[idx];
        while (*curr != Entity::Null()) {
            if (*curr == handle) {
                *curr              = comp->next_in_grid;
                comp->next_in_grid = Entity::Null();
                break;
            }
            auto* curr_comp = entities.Get<ALifeComponent>(*curr);
            if (curr_comp == nullptr) break;
            curr = &curr_comp->next_in_grid;
        }
    }
}

auto SpatialGrid::Query(const COM::IEntities& entities, DVec3 pos, float radius, std::vector<Entity> reusable) const -> std::vector<Entity> {
    reusable.clear();
    const auto cell_size = static_cast<double>(_cellSize);
    const auto rad       = static_cast<double>(radius);
    const auto min_x     = static_cast<int32_t>(std::floor((pos.x - rad) / cell_size));
    const auto max_x     = static_cast<int32_t>(std::floor((pos.x + rad) / cell_size));
    const auto min_z     = static_cast<int32_t>(std::floor((pos.z - rad) / cell_size));
    const auto max_z     = static_cast<int32_t>(std::floor((pos.z + rad) / cell_size));
    const double radius_sq = rad * rad;

    for (int32_t z = min_z; z <= max_z; ++z) {
        for (int32_t x = min_x; x <= max_x; ++x) {
            if (x < 0 || x >= static_cast<int32_t>(_width) || z < 0 || z >= static_cast<int32_t>(_height)) continue;
            Entity current = _cellHeads[(z * static_cast<int32_t>(_width)) + x];
            while (current != Entity::Null()) {
                const auto* comp = entities.Get<ALifeComponent>(current);
                if (comp == nullptr) break;
                if ((comp->position - pos).LengthSquared() <= radius_sq) {
                    reusable.push_back(current);
                }
                current = comp->next_in_grid;
            }
        }
    }
    return reusable;
}

} // namespace ZHLN::ALife
