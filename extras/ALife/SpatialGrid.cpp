// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpatialGrid.hpp"
#include "ALifeComponents.hpp"
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cmath>

namespace ZHLN::ALife {

SpatialGrid::SpatialGrid(uint32_t w, uint32_t h, float cell_size): _width(w), _height(h), _cellSize(cell_size) {
    _cellHeads.resize(static_cast<size_t>(w) * h, Entity::Null());
}

void SpatialGrid::Clear() noexcept {
    std::ranges::fill(_cellHeads, Entity::Null());
}

auto SpatialGrid::GetCellIndex(JPH::RVec3Arg pos) const noexcept -> int32_t {
    if (pos.GetX() < 0.0 || pos.GetZ() < 0.0) {
        return -1;
    }

    const auto cell_size = static_cast<double>(_cellSize);
    auto       gx        = static_cast<int32_t>(pos.GetX() / cell_size);
    auto       gz        = static_cast<int32_t>(pos.GetZ() / cell_size);

    if (gx >= static_cast<int32_t>(_width) || gz >= static_cast<int32_t>(_height)) {
        return -1;
    }
    return (gz * static_cast<int32_t>(_width)) + gx;
}

void SpatialGrid::UpdateEntity(ECS::Registry& reg, Entity handle, JPH::RVec3Arg old_pos) {
    auto* comp = reg.Get<ALifeComponent>(handle);
    if (comp == nullptr) {
        return;
    }

    // Cache the entity handle inside the component if not already done
    if (comp->self_entity == Entity::Null()) {
        comp->self_entity = handle;
    }

    const int32_t old_idx = GetCellIndex(old_pos);
    const int32_t new_idx = GetCellIndex(comp->position);

    if (old_idx == new_idx) {
        return;
    }

    // Unlink using the full generational handle. Index-only lookups used to
    // fabricate generation 0, which SparseSet::Get correctly rejects.
    if (old_idx != -1) {
        Entity* curr = &_cellHeads[old_idx];
        while (*curr != Entity::Null()) {
            if (*curr == handle) {
                *curr = comp->next_in_grid;
                break;
            }
            auto* curr_comp = reg.Get<ALifeComponent>(*curr);
            if (curr_comp == nullptr) {
                break;
            }
            curr = &curr_comp->next_in_grid;
        }
    }

    comp->next_in_grid = Entity::Null();
    if (new_idx != -1) {
        comp->next_in_grid  = _cellHeads[new_idx];
        _cellHeads[new_idx] = handle;
    }
}

void SpatialGrid::RemoveEntity(ECS::Registry& reg, Entity handle) {
    auto* comp = reg.Get<ALifeComponent>(handle);
    if (comp == nullptr) {
        return;
    }

    const int32_t idx = GetCellIndex(comp->position);
    if (idx != -1) {
        Entity* curr = &_cellHeads[idx];
        while (*curr != Entity::Null()) {
            if (*curr == handle) {
                *curr = comp->next_in_grid;
                comp->next_in_grid = Entity::Null();
                break;
            }
            auto* curr_comp = reg.Get<ALifeComponent>(*curr);
            if (curr_comp == nullptr) {
                break;
            }
            curr = &curr_comp->next_in_grid;
        }
    }
}

auto SpatialGrid::Query(const ECS::Registry& reg, JPH::RVec3Arg pos, float radius, std::vector<Entity>& out_buffer) const -> uint32_t {
    uint32_t count = 0;

    const auto cell_size = static_cast<double>(_cellSize);
    const auto rad       = static_cast<double>(radius);

    auto min_x = static_cast<int32_t>(std::floor((pos.GetX() - rad) / cell_size));
    auto max_x = static_cast<int32_t>(std::floor((pos.GetX() + rad) / cell_size));
    auto min_z = static_cast<int32_t>(std::floor((pos.GetZ() - rad) / cell_size));
    auto max_z = static_cast<int32_t>(std::floor((pos.GetZ() + rad) / cell_size));

    const float radius_sq = radius * radius;

    for (int32_t z = min_z; z <= max_z; ++z) {
        for (int32_t x = min_x; x <= max_x; ++x) {
            // Grid boundary clipping
            if (x < 0 || x >= static_cast<int32_t>(_width) || z < 0 || z >= static_cast<int32_t>(_height)) {
                continue;
            }

            Entity current = _cellHeads[(z * static_cast<int32_t>(_width)) + x];
            while (current != Entity::Null()) {
                const auto* comp = reg.Get<ALifeComponent>(current);
                if (comp == nullptr) {
                    break; // A destroyed component cannot supply the next link.
                }

                const float dist_sq = (comp->position - pos).LengthSq();
                if (dist_sq <= radius_sq) {
                    out_buffer.push_back(current);
                    count++;
                }

                current = comp->next_in_grid;
            }
        }
    }
    return count;
}

} // namespace ZHLN::ALife
