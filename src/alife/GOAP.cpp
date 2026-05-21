#include <Zahlen/alife/GOAP.hpp>
#include <algorithm>
#include <bit>
#include <limits>

namespace ZHLN::ALife {

constexpr size_t GOAP_MAX_NODES = 256;

// Internal State-Space A* Node
struct GOAPNode {
	WorldState state;
	uint32_t g_score;
	uint32_t f_score;
	uint32_t parent_idx;
	uint32_t action_idx;
};

// Internal Helper: Open List Min-Heap
struct GOAPHeap {
	uint32_t* heap;
	uint32_t& count;
	const GOAPNode* pool;

	void Push(uint32_t node_idx) noexcept {
		uint32_t i = count++;
		while (i > 0) {
			uint32_t p = (i - 1) / 2;
			if (pool[heap[p]].f_score <= pool[node_idx].f_score) {
				break;
			}
			heap[i] = heap[p];
			i = p;
		}
		heap[i] = node_idx;
	}

	uint32_t Pop() noexcept {
		uint32_t top = heap[0];
		uint32_t last = heap[--count];
		uint32_t i = 0;
		while (i * 2 + 1 < count) {
			uint32_t child = i * 2 + 1;
			if (child + 1 < count && pool[heap[child + 1]].f_score < pool[heap[child]].f_score) {
				child++;
			}
			if (pool[last].f_score <= pool[heap[child]].f_score) {
				break;
			}
			heap[i] = heap[child];
			i = child;
		}
		heap[i] = last;
		return top;
	}

	[[nodiscard]] bool Empty() const noexcept { return count == 0; }
};

// --- Heuristic & State Math ---

static inline uint32_t CalculateHeuristic(WorldState current, WorldState goal) noexcept {
	uint64_t unmet_bits = (current.values ^ goal.values) & goal.mask;

	// Modern C++23 standard way to get fast hardware popcount
	return static_cast<uint32_t>(std::popcount(unmet_bits));
}

static inline WorldState ApplyAction(WorldState state, WorldState effects) noexcept {
	WorldState next = state;
	next.values = (next.values & ~effects.mask) | (effects.values & effects.mask);
	next.mask |= effects.mask;
	return next;
}

// --- The A* Solver ---

Plan SolvePlan(const PlanRequest& request, const std::vector<Action>& actions) {
	// Allocation-free static pools (Perfect for Fiber task safety)
	GOAPNode pool[GOAP_MAX_NODES];
	uint32_t pool_count = 0;

	uint32_t open_list[GOAP_MAX_NODES];
	uint32_t open_count = 0;

	GOAPHeap heap{.heap = open_list, .count = open_count, .pool = pool};

	// Initialize Root Node
	pool[0] = GOAPNode{.state = request.current,
					   .g_score = 0,
					   .f_score = CalculateHeuristic(request.current, request.goal),
					   .parent_idx = 0xFFFFFFFF,
					   .action_idx = 0xFFFFFFFF};
	pool_count++;
	heap.Push(0);

	uint32_t best_goal_idx = 0xFFFFFFFF;

	// A* State-Space Search Loop
	while (!heap.Empty()) {
		uint32_t curr_idx = heap.Pop();
		GOAPNode& curr_node = pool[curr_idx];

		if (curr_node.state.Matches(request.goal)) {
			best_goal_idx = curr_idx;
			break;
		}

		for (uint32_t i = 0; i < static_cast<uint32_t>(actions.size()); ++i) {
			const Action& action = actions[i];

			if (!curr_node.state.Matches(action.preconditions)) {
				continue;
			}

			WorldState next_state = ApplyAction(curr_node.state, action.effects);
			uint32_t tentative_g = curr_node.g_score + action.cost;

			bool state_exists = false;
			for (uint32_t j = 0; j < pool_count; ++j) {
				if (pool[j].state.values == next_state.values &&
					pool[j].state.mask == next_state.mask) {
					state_exists = true;
					if (tentative_g < pool[j].g_score) {
						pool[j].g_score = tentative_g;
						pool[j].f_score =
							tentative_g + CalculateHeuristic(next_state, request.goal);
						pool[j].parent_idx = curr_idx;
						pool[j].action_idx = i;
						heap.Push(j);
					}
					break;
				}
			}

			if (!state_exists && pool_count < GOAP_MAX_NODES) {
				uint32_t new_idx = pool_count++;
				pool[new_idx] =
					GOAPNode{.state = next_state,
							 .g_score = tentative_g,
							 .f_score = tentative_g + CalculateHeuristic(next_state, request.goal),
							 .parent_idx = curr_idx,
							 .action_idx = i};
				heap.Push(new_idx);
			}
		}
	}

	// Plan Reconstruction (Backtrack from Goal)
	Plan plan{};
	if (best_goal_idx != 0xFFFFFFFF) {
		uint32_t curr = best_goal_idx;
		const Action* temp_actions[MAX_PLAN_LENGTH];
		uint32_t temp_count = 0;

		while (pool[curr].parent_idx != 0xFFFFFFFF && temp_count < MAX_PLAN_LENGTH) {
			temp_actions[temp_count++] = &actions[pool[curr].action_idx];
			curr = pool[curr].parent_idx;
		}

		// Reverse plan to yield chronological execution order
		for (uint32_t i = 0; i < temp_count; ++i) {
			plan.actions[i] = *temp_actions[temp_count - 1 - i];
		}
		plan.count = temp_count;
	}

	return plan;
}

// --- Registry Implementation ---

uint32_t WorldStateRegistry::RegisterKey(std::string_view name) {
	for (uint32_t i = 0; i < static_cast<uint32_t>(_keyNames.size()); ++i) {
		if (_keyNames[i] == name) {
			return i;
		}
	}

	if (_keyNames.size() < MAX_GOAP_STATES) {
		uint32_t id = static_cast<uint32_t>(_keyNames.size());
		_keyNames.push_back(String32(name));
		return id;
	}

	return 0xFFFFFFFF; // Registry Full
}

uint32_t WorldStateRegistry::GetID(std::string_view name) const {
	for (uint32_t i = 0; i < static_cast<uint32_t>(_keyNames.size()); ++i) {
		if (_keyNames[i] == name) {
			return i;
		}
	}
	return 0xFFFFFFFF;
}

} // namespace ZHLN::ALife