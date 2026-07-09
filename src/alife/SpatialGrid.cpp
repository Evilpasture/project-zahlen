// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Components.hpp>
#include <Zahlen/alife/SpatialGrid.hpp>
#include <cmath>
#include <ecs/ECS.hpp>

namespace ZHLN::ALife {

SpatialGrid::SpatialGrid(uint32_t w, uint32_t h, float cell_size)
	: _width(w), _height(h), _cellSize(cell_size) {
	_cellHeads.resize(static_cast<size_t>(w) * h, END_OF_LIST);
}

void SpatialGrid::Clear() noexcept {
	std::fill(_cellHeads.begin(), _cellHeads.end(), END_OF_LIST);
}

int32_t SpatialGrid::GetCellIndex(JPH::RVec3Arg pos) const noexcept {
	if (pos.GetX() < 0.0f || pos.GetZ() < 0.0f) {
		return -1;
	}

	int32_t gx = static_cast<int32_t>(pos.GetX() / _cellSize);
	int32_t gz = static_cast<int32_t>(pos.GetZ() / _cellSize);

	if (gx >= static_cast<int32_t>(_width) || gz >= static_cast<int32_t>(_height)) {
		return -1;
	}
	return (gz * static_cast<int32_t>(_width)) + gx;
}

void SpatialGrid::UpdateEntity(ECS::Registry& reg, Entity handle, JPH::RVec3Arg old_pos) {
	auto* comp = reg.Get<Components::ALifeComponent>(handle);
	if (!comp)
		return;

	// Cache the entity handle inside the component if not already done
	if (comp->self_entity == NullEntity) {
		comp->self_entity = handle;
	}

	int32_t old_idx = GetCellIndex(old_pos);
	int32_t new_idx = GetCellIndex(comp->position);

	if (old_idx == new_idx)
		return;

	// 1. Unlink from old cell list
	if (old_idx != -1) {
		uint32_t* curr = &_cellHeads[old_idx];
		while (*curr != END_OF_LIST) {
			if (*curr == handle.index) {
				Entity dummy{.index = *curr, .generation = 0};
				auto* curr_comp = reg.Get<Components::ALifeComponent>(dummy);
				*curr = curr_comp ? curr_comp->next_in_grid : END_OF_LIST;
				break;
			}
			Entity dummy{.index = *curr, .generation = 0};
			auto* curr_comp = reg.Get<Components::ALifeComponent>(dummy);
			if (!curr_comp)
				break;
			curr = &curr_comp->next_in_grid;
		}
	}

	// 2. Link to new cell list
	if (new_idx != -1) {
		comp->next_in_grid = _cellHeads[new_idx];
		_cellHeads[new_idx] = handle.index;
	}
}

void SpatialGrid::RemoveEntity(ECS::Registry& reg, Entity handle) {
	auto* comp = reg.Get<Components::ALifeComponent>(handle);
	if (!comp)
		return;

	int32_t idx = GetCellIndex(comp->position);
	if (idx != -1) {
		uint32_t* curr = &_cellHeads[idx];
		while (*curr != END_OF_LIST) {
			if (*curr == handle.index) {
				Entity dummy{.index = handle.index, .generation = 0};
				auto* curr_comp = reg.Get<Components::ALifeComponent>(dummy);

				*curr = curr_comp ? curr_comp->next_in_grid : END_OF_LIST;

				if (curr_comp) {
					curr_comp->next_in_grid = END_OF_LIST;
				}
				break;
			}
			Entity dummy{.index = *curr, .generation = 0};
			auto* curr_comp = reg.Get<Components::ALifeComponent>(dummy);
			if (!curr_comp)
				break;
			curr = &curr_comp->next_in_grid;
		}
	}
}

uint32_t SpatialGrid::Query(const ECS::Registry& reg, JPH::RVec3Arg pos, float radius,
							std::vector<Entity>& out_buffer) const {
	uint32_t count = 0;

	int32_t min_x = static_cast<int32_t>(std::floor((pos.GetX() - radius) / _cellSize));
	int32_t max_x = static_cast<int32_t>(std::floor((pos.GetX() + radius) / _cellSize));
	int32_t min_z = static_cast<int32_t>(std::floor((pos.GetZ() - radius) / _cellSize));
	int32_t max_z = static_cast<int32_t>(std::floor((pos.GetZ() + radius) / _cellSize));

	const float radius_sq = radius * radius;

	for (int32_t z = min_z; z <= max_z; ++z) {
		for (int32_t x = min_x; x <= max_x; ++x) {
			// Grid boundary clipping
			if (x < 0 || x >= static_cast<int32_t>(_width) || z < 0 ||
				z >= static_cast<int32_t>(_height)) {
				continue;
			}

			uint32_t slot_idx = _cellHeads[(z * static_cast<int32_t>(_width)) + x];
			while (slot_idx != END_OF_LIST) {
				// Generational bypass: we can lookup safely by index only
				Entity dummy{.index = slot_idx, .generation = 0};
				auto* comp = reg.Get<Components::ALifeComponent>(dummy);
				if (!comp)
					break;

				float dist_sq = (comp->position - pos).LengthSq();
				if (dist_sq <= radius_sq) {
					// Fetch actual entity with correct generation from the component
					out_buffer.push_back(comp->self_entity);
					count++;
				}

				slot_idx = comp->next_in_grid;
			}
		}
	}
	return count;
}

} // namespace ZHLN::ALife
