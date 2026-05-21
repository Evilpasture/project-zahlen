#include <Zahlen/Log.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/alife/Simulator.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <threading/TaskSystem.hpp>

namespace ZHLN::ALife {

// --- Internal Math Helper ---
static JPH::RVec3 MoveTowards(JPH::RVec3Arg current, JPH::RVec3Arg target, float max_dist) {
	JPH::Vec3 diff = target - current; // RVec3 - RVec3 yields a local float Vec3
	float d2 = diff.LengthSq();

	if (d2 < (max_dist * max_dist) || d2 < 0.0001f) {
		return target;
	}

	float d = std::sqrt(d2);
	return current + (diff / d) * max_dist;
}

// --- Simulator Initialization ---

Simulator::Simulator(const SimConfig& config)
	: _grid(config.grid_width, config.grid_height, config.cell_size),
	  _levelGraph(0), // Normally populated after creation
	  _factionRegistry(config.max_factions), _tuning(config.default_tuning) {}

void Simulator::SetRelation(uint32_t a, uint32_t b, float value) {
	_factionRegistry.SetRelation(a, b, value);
}

void Simulator::BroadcastEvent(const Event& event) {
	if (on_event) {
		on_event(*this, event);
	}
}

// --- Main Simulation Loop ---

void Simulator::Update(Engine& engine, float dt, JPH::RVec3Arg observer_pos) {
	ZHLN_PROFILE_SCOPE("ALife Simulator Update");

	// 1. Advance Game Time
	_gameTimeMS += static_cast<uint64_t>(dt * 1000.0f * _tuning.time_factor);

	const float game_dt = dt * _tuning.time_factor;
	const float dist_sq_threshold = _tuning.switch_distance * _tuning.switch_distance;

	ECS::Registry& reg = engine.GetRegistry();
	std::span<const Entity> entities = reg.GetEntitiesWith<ALifeComponent>();
	RestrictSpan<ALifeComponent> comps = reg.GetRawArray<ALifeComponent>();

	if (entities.empty())
		return;

	// --- PHASE 1: MOVEMENT & STATE SWITCHING (Parallel) ---
	TaskSystem::ParallelFor(entities.size(), 256, [&](uint32_t start, uint32_t end, uint32_t) {
		for (uint32_t i = start; i < end; ++i) {
			Entity e = entities[i];
			ALifeComponent& comp = comps[i];

			if (comp.state == State::Dead)
				continue;

			// Handle Waiting
			if (comp.wait_time > 0) {
				comp.wait_time -= static_cast<int32_t>(game_dt);
				continue;
			}

			// Trigger "Think" callback if idle
			if (comp.target_node == INVALID_GRAPH_NODE && comp.path_count == 0 && on_think) {
				on_think(*this, e);
			}

			// Movement along path (A*)
			if (comp.path_index < comp.path_count && _levelGraph.GetNodeCount() > 0) {
				uint32_t target_node = comp.path[comp.path_index];
				JPH::RVec3 target_pos = _levelGraph.GetNode(target_node).position;

				float speed = comp.travel_speed;
				if (comp.is_fleeing)
					speed *= 1.5f;

				comp.position = MoveTowards(comp.position, target_pos, speed * game_dt);

				if (comp.position.IsClose(target_pos, 0.01f)) {
					comp.current_node = target_node;
					comp.path_index++;

					if (comp.path_index >= comp.path_count) {
						comp.path_count = 0;
						if (on_task_completed)
							on_task_completed(*this, e);
					}
				}
			}
			// Fallback: Direct Node Movement
			else if (comp.target_node != INVALID_GRAPH_NODE && _levelGraph.GetNodeCount() > 0) {
				JPH::RVec3 target_pos = _levelGraph.GetNode(comp.target_node).position;

				float speed = comp.travel_speed;
				if (comp.is_fleeing)
					speed *= 1.5f;

				comp.position = MoveTowards(comp.position, target_pos, speed * game_dt);

				if (comp.position.IsClose(target_pos, 0.01f)) {
					comp.current_node = comp.target_node;
					comp.target_node = INVALID_GRAPH_NODE;
					if (on_task_completed)
						on_task_completed(*this, e);
				}
			}

			// Distance-based Online/Offline State Switching
			float d2 = (comp.position - observer_pos).LengthSq();
			State old_state = comp.state;

			if (comp.state == State::Offline && d2 < dist_sq_threshold) {
				comp.state = State::Online;
			} else if (comp.state == State::Online && d2 > dist_sq_threshold) {
				comp.state = State::Offline;
			}

			if (comp.state != old_state) {
				// Thread-safe if event callback uses ZHLN_LOCK or concurrent queues
				BroadcastEvent(
					{EventType::StateChange, e, {.state_change = {old_state, comp.state}}});
			}
		}
	});

	// --- PHASE 2: SPATIAL GRID REBUILD (Serial) ---
	// Fast O(N) re-indexing of all entities into the spatial grid
	_grid.Clear();
	for (size_t i = 0; i < entities.size(); ++i) {
		_grid.UpdateEntity(reg, entities[i], JPH::RVec3(-1, -1, -1)); // -1 forces fresh insertion
	}

	// --- PHASE 3: OFFLINE INTERACTIONS (Parallel) ---
	if (on_interaction) {
		TaskSystem::ParallelFor(entities.size(), 256, [&](uint32_t start, uint32_t end, uint32_t) {
			std::vector<Entity> neighbors;
			neighbors.reserve(8);

			for (uint32_t i = start; i < end; ++i) {
				Entity e = entities[i];
				ALifeComponent& comp = comps[i];

				if (comp.state != State::Offline || comp.state == State::Dead)
					continue;

				neighbors.clear();
				_grid.Query(reg, comp.position, 50.0f, neighbors);

				for (Entity neighbor : neighbors) {
					if (neighbor != e) {
						on_interaction(*this, e, neighbor);
						break; // Only interact with one entity per tick
					}
				}
			}
		});
	}

	_currentTickIdx++;
}

// --- Interaction Math ---

void Simulator::ResolveOfflineInteraction(ECS::Registry& reg, Entity e1, Entity e2) {
	auto* c1 = reg.Get<ALifeComponent>(e1);
	auto* c2 = reg.Get<ALifeComponent>(e2);
	if (!c1 || !c2)
		return;

	if (c1->state != State::Dead && c2->state != State::Dead) {
		float rel = _factionRegistry.GetRelation(c1->faction_id, c2->faction_id);

		if (rel < -0.5f) {
			// Combat Roll
			if ((std::rand() % 100) > _tuning.combat_hit_chance)
				return;

			int32_t dmg1 = (c1->power / 4) + (std::rand() % 5);
			int32_t dmg2 = (c2->power / 4) + (std::rand() % 5);

			c1->health -= dmg2;
			c2->health -= dmg1;

			if (c1->health < _tuning.combat_flee_threshold && c1->health > 0)
				c1->is_fleeing = true;
			if (c2->health < _tuning.combat_flee_threshold && c2->health > 0)
				c2->is_fleeing = true;

			c1->wait_time = _tuning.combat_wait_time;
			c2->wait_time = _tuning.combat_wait_time;

			if (c1->health <= 0) {
				c1->state = State::Dead;
				c1->health = 0;
				c1->loot_value = std::rand() % 50 + 10;
				BroadcastEvent({EventType::Death, e1, {.death = {e2}}});
			}

			if (c2->health <= 0) {
				c2->state = State::Dead;
				c2->health = 0;
				c2->loot_value = std::rand() % 50 + 10;
				BroadcastEvent({EventType::Death, e2, {.death = {e1}}});
			}
		}
	}
	// Scavenging: E1 loots E2
	else if (c1->state != State::Dead && c2->state == State::Dead && !c2->is_looted) {
		c1->power += (c2->loot_value / 10);
		c2->is_looted = true;
		_grid.RemoveEntity(reg, e2);
	}
	// Scavenging: E2 loots E1
	else if (c2->state != State::Dead && c1->state == State::Dead && !c1->is_looted) {
		c2->power += (c1->loot_value / 10);
		c1->is_looted = true;
		_grid.RemoveEntity(reg, e1);
	}
}

// --- Serialization ---

struct SaveHeader {
	uint32_t magic = 0x414C4946; // "ALIF"
	uint32_t version = 1;
	uint32_t entity_count;
};

struct SaveRecord {
	Entity entity;
	ALifeComponent comp;
};

bool Simulator::Save(const char* filename) const {
	// Note: This only saves the ALife component data, not the full ECS registry.
	// In a real engine, ECS serialization is usually handled holistically.
	// But this fulfills the A-Life specific save requirement.

	// (Wait, I need the registry to get the components. The header signature
	// `bool Save(const char* filename) const` doesn't pass the Registry!
	// Let me update the header signature in your actual code to `bool Save(ECS::Registry& reg,
	// const char* filename) const;` For now, if the registry isn't passed, I can't save. I'll just
	// return false here and advise updating the header.)
	ZHLN::Log(
		"ERROR: Simulator::Save requires an ECS::Registry reference to serialize ALifeComponents!");
	return false;
}

bool Simulator::Load(ECS::Registry& reg, const char* filename) {
	std::ifstream file(filename, std::ios::binary);
	if (!file.is_open())
		return false;

	SaveHeader header;
	file.read(reinterpret_cast<char*>(&header), sizeof(SaveHeader));

	if (header.magic != 0x414C4946 || header.version != 1) {
		return false;
	}

	// Since we are restoring components to specific Entities,
	// the ECS Registry must be in the exact same state (same entity generations).
	// Usually, you clear the ECS and rebuild it from the save file.
	for (uint32_t i = 0; i < header.entity_count; ++i) {
		SaveRecord rec;
		file.read(reinterpret_cast<char*>(&rec), sizeof(SaveRecord));

		// Either update existing or add new
		if (reg.IsAlive(rec.entity)) {
			if (auto* existing = reg.Get<ALifeComponent>(rec.entity)) {
				*existing = rec.comp;
			} else {
				reg.Add(rec.entity, std::move(rec.comp));
			}
		}
	}

	_grid.Clear();
	auto entities = reg.GetEntitiesWith<ALifeComponent>();
	for (Entity e : entities) {
		_grid.UpdateEntity(reg, e, JPH::RVec3(-1, -1, -1));
	}

	return true;
}

} // namespace ZHLN::ALife