#pragma once

#include "Types.hpp"

#include <Zahlen/Entity.hpp>
#include <vector>

// Forward declare engine registry
namespace ZHLN::ECS {
class Registry;
}

namespace ZHLN::ALife {

class SpatialGrid {
  public:
	SpatialGrid(uint32_t w, uint32_t h, float cell_size);

	void Clear() noexcept;
	void UpdateEntity(ECS::Registry& reg, Entity handle, JPH::RVec3Arg old_pos);
	void RemoveEntity(ECS::Registry& reg, Entity handle);

	// Returns number of items found and populates out_buffer
	uint32_t Query(const ECS::Registry& reg, JPH::RVec3Arg pos, float radius,
				   std::vector<Entity>& out_buffer) const;

	[[nodiscard]] int32_t GetCellIndex(JPH::RVec3Arg pos) const noexcept;

	std::vector<uint32_t> _cellHeads;
	uint32_t _width;
	uint32_t _height;
	float _cellSize;
};

} // namespace ZHLN::ALife