// include/Zahlen/physics/ArticulationSystem.hpp
#pragma once

#include <Zahlen/Common.h>

namespace ZHLN {

class Engine;

class ZHLN_API ArticulationSystem {
  public:
	ArticulationSystem() = default;
	~ArticulationSystem() = default;

	// Non-copyable to prevent accidental duplication of internal system state
	ArticulationSystem(const ArticulationSystem&) = delete;
	ArticulationSystem& operator=(const ArticulationSystem&) = delete;

	/**
	 * @brief Evaluates active motor forces on dynamic ragdoll constraints
	 * and synchronizes simulated body poses back to the visual mesh joint palette.
	 */
	void Update(Engine& engine, float dt);
};

} // namespace ZHLN
