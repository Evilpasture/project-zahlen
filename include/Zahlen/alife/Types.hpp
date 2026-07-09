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
static constexpr uint32_t INVALID_GRAPH_NODE = 0xFFFFFFFF;
static constexpr size_t MAX_PATH_LENGTH = 16;
static constexpr uint32_t END_OF_LIST = 0xFFFFFFFF;
enum class State : uint8_t { Offline, Online, Dead };
enum class TaskType : uint8_t { Idle = 0, GotoHub, Patrol, Hunt };
enum class EventType : uint8_t { StateChange, Death, NodeReached };

#ifdef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnested-anon-types"
#endif
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
#ifdef __clang__
#pragma GCC diagnostic pop
#endif

struct PathRequest {
	Entity entity;
	uint32_t start_node;
	uint32_t end_node;

	uint32_t path[MAX_PATH_LENGTH]{};
	uint32_t path_count = 0;
	bool success = false;
};

} // namespace ZHLN::ALife
