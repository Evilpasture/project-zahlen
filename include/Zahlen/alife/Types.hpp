// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Entity.hpp>

// Correct Jolt Includes for RVec3 / RVec3Arg
#include <Jolt/Jolt.h>
#include <Jolt/Math/DVec3.h>
#include <Jolt/Math/Real.h>
#include <Jolt/Math/Vec3.h>
#include <cstdint>

namespace ZHLN::ALife {

constexpr uint32_t INVALID_GRAPH_NODE = 0xFFFFFFFF;
constexpr size_t MAX_PATH_LENGTH = 16;
constexpr uint32_t END_OF_LIST = 0xFFFFFFFF;

enum class State : uint8_t { Offline, Online, Dead };
enum class TaskType : uint8_t { Idle = 0, GotoHub, Patrol, Hunt };
enum class EventType : uint8_t { StateChange, Death, NodeReached };

struct Event {
	EventType type;
	Entity subject;
	union {
		struct {
			State old_state;
			State new_state;
		} state_change;
		struct {
			Entity killer;
		} death;
		struct {
			uint32_t node_index;
		} node_reached;
	};
};

/**
 * @brief Native Zahlen ECS Component for Artificial Life.
 * Public struct to allow C++20 designated initializers.
 */
struct ALifeComponent {
	// --- Simulator Mandatory Fields ---
	JPH::RVec3 position = JPH::RVec3::sZero();
	State state = State::Offline;
	uint32_t current_node = INVALID_GRAPH_NODE;
	uint32_t target_node = INVALID_GRAPH_NODE;
	float travel_speed = 0.0f;
	uint32_t faction_id = 0;
	Entity self_entity = NullEntity;

	uint32_t path[MAX_PATH_LENGTH]{};
	uint32_t path_count = 0;
	uint32_t path_index = 0;

	int32_t wait_time = 0;
	bool is_thinking = false;

	// Spatial Grid Intrusive Linked List
	uint32_t next_in_grid = END_OF_LIST;

	// --- User Fields ---
	uint32_t class_id = 0;
	int32_t health = 100;
	int32_t power = 10;
	int32_t money = 0;
	int32_t energy = 100;
	int32_t loot_value = 0;
	TaskType active_task = TaskType::Idle;
	bool is_looted = false;
	bool is_fleeing = false;
	uint64_t script_handle = 0;
};

struct PathRequest {
	Entity entity;
	uint32_t start_node;
	uint32_t end_node;

	uint32_t path[MAX_PATH_LENGTH]{};
	uint32_t path_count = 0;
	bool success = false;
};

} // namespace ZHLN::ALife