#pragma once

#include "Factions.hpp"
#include "Graph.hpp"
#include "SpatialGrid.hpp"
#include "Types.hpp"

#include <Zahlen/Engine.hpp>
#include <functional>

namespace ZHLN::ALife {

struct SimTuning {
	float time_factor = 10.0f;
	float switch_distance = 150.0f;
	uint32_t objects_per_tick = 100;
	int32_t combat_hit_chance = 30;
	int32_t combat_flee_threshold = 30;
	int32_t combat_wait_time = 2;
};

struct SimConfig {
	uint32_t max_factions = 32;
	uint32_t grid_width = 100;
	uint32_t grid_height = 100;
	float cell_size = 50.0f;
	SimTuning default_tuning;
};

class Simulator {
  public:
	explicit Simulator(const SimConfig& config = SimConfig{});

	// Subsystem update hook
	void Update(Engine& engine, float dt, JPH::RVec3Arg observer_pos);

	void SetRelation(uint32_t a, uint32_t b, float value);
	void BroadcastEvent(const Event& event);
	void ResolveOfflineInteraction(ECS::Registry& reg, Entity e1, Entity e2);

	bool Save(const char* filename) const;
	bool Load(ECS::Registry& reg, const char* filename);

	// Callbacks
	std::function<void(Simulator&, Entity, Entity)> on_interaction;
	std::function<void(Simulator&, const Event&)> on_event;
	std::function<void(Simulator&, Entity)> on_think;
	std::function<void(Simulator&, Entity)> on_task_completed;

	// Encapsulated state accessors
	[[nodiscard]] SpatialGrid& GetGrid() noexcept { return _grid; }
	[[nodiscard]] LevelGraph& GetGraph() noexcept { return _levelGraph; }
	[[nodiscard]] FactionRegistry& GetFactions() noexcept { return _factionRegistry; }

	[[nodiscard]] SimTuning& GetTuning() noexcept { return _tuning; }
	[[nodiscard]] const SimTuning& GetTuning() const noexcept { return _tuning; }

	[[nodiscard]] uint64_t GetGameTimeMS() const noexcept { return _gameTimeMS; }

  private:
	SpatialGrid _grid;
	LevelGraph _levelGraph;
	FactionRegistry _factionRegistry;
	SimTuning _tuning;

	uint64_t _gameTimeMS = 0;
	uint32_t _currentTickIdx = 0;
};

} // namespace ZHLN::ALife