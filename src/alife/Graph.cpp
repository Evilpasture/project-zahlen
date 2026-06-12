// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include <Zahlen/alife/Graph.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace ZHLN::ALife {

// --- Internal Helper: Thread-Safe Min-Heap ---
// Keeps heap-sorting operations completely hidden from the public header
struct MinHeap {
	std::pmr::vector<HeapNode>& data;
	uint32_t count = 0;

	void Push(uint32_t node_idx, float f_score) noexcept {
		uint32_t i = count++;
		while (i > 0) {
			uint32_t p = (i - 1) / 2;
			if (data[p].f_score <= f_score) {
				break;
			}
			data[i] = data[p];
			i = p;
		}
		data[i] = HeapNode{node_idx, f_score};
	}

	HeapNode Pop() noexcept {
		HeapNode top = data[0];
		HeapNode last = data[--count];
		uint32_t i = 0;
		while (i * 2 + 1 < count) {
			uint32_t child = i * 2 + 1;
			if (child + 1 < count && data[child + 1].f_score < data[child].f_score) {
				child++;
			}
			if (last.f_score <= data[child].f_score) {
				break;
			}
			data[i] = data[child];
			i = child;
		}
		data[i] = last;
		return top;
	}

	[[nodiscard]] bool Empty() const noexcept { return count == 0; }
};

// --- Level Graph Implementation ---

LevelGraph::LevelGraph(uint32_t node_count) {
	_nodes.resize(node_count);
	for (auto& node : _nodes) {
		std::fill(std::begin(node.neighbors), std::end(node.neighbors), INVALID_GRAPH_NODE);
		node.neighbor_count = 0;
	}
}

void LevelGraph::Connect(uint32_t a, uint32_t b) {
	if (a >= _nodes.size() || b >= _nodes.size()) {
		return;
	}

	Node& node_a = _nodes[a];
	if (node_a.neighbor_count < MAX_NODE_NEIGHBORS) {
		node_a.neighbors[node_a.neighbor_count++] = b;
	}

	Node& node_b = _nodes[b];
	if (node_b.neighbor_count < MAX_NODE_NEIGHBORS) {
		node_b.neighbors[node_b.neighbor_count++] = a;
	}
}

uint32_t LevelGraph::FindClosest(JPH::RVec3Arg pos) const {
	uint32_t best_idx = INVALID_GRAPH_NODE;
	float min_dist_sq = std::numeric_limits<float>::max();

	for (uint32_t i = 0; i < _nodes.size(); ++i) {
		// High-performance SIMD subtraction and squared length check from Jolt
		float d2 = (_nodes[i].position - pos).LengthSq();

		if (d2 < min_dist_sq) {
			min_dist_sq = d2;
			best_idx = i;
		}
	}
	return best_idx;
}

uint32_t LevelGraph::FindPath(uint32_t start, uint32_t end, uint32_t* out_path,
							  PathWorkspace& ws) const {
	if (start == end || start >= _nodes.size() || end >= _nodes.size()) {
		return 0;
	}
	if (_nodes.size() > ws.node_data.size()) {
		return 0; // Guard against Workspace overflow
	}

	// Reset A* workspace values for this run
	std::fill(ws.node_data.begin(), ws.node_data.begin() + _nodes.size(),
			  AStarData{.g_score = std::numeric_limits<float>::infinity(),
						.parent = INVALID_GRAPH_NODE,
						.closed = false});

	MinHeap open_list{.data = ws.heap_mem, .count = 0};

	ws.node_data[start].g_score = 0;

	// Distances use Jolt's built-in vector length operator
	float initial_heuristic = (_nodes[start].position - _nodes[end].position).Length();
	open_list.Push(start, initial_heuristic);

	bool found = false;
	while (!open_list.Empty()) {
		HeapNode current = open_list.Pop();

		if (current.node_idx == end) {
			found = true;
			break;
		}

		if (ws.node_data[current.node_idx].closed) {
			continue;
		}
		ws.node_data[current.node_idx].closed = true;

		const Node& graph_node = _nodes[current.node_idx];
		for (uint32_t i = 0; i < graph_node.neighbor_count; ++i) {
			uint32_t neighbor_idx = graph_node.neighbors[i];
			if (neighbor_idx >= _nodes.size() || ws.node_data[neighbor_idx].closed) {
				continue;
			}

			float weight = (graph_node.position - _nodes[neighbor_idx].position).Length();
			float tentative_g = ws.node_data[current.node_idx].g_score + weight;

			if (tentative_g < ws.node_data[neighbor_idx].g_score) {
				ws.node_data[neighbor_idx].parent = current.node_idx;
				ws.node_data[neighbor_idx].g_score = tentative_g;

				float f_score =
					tentative_g + (_nodes[neighbor_idx].position - _nodes[end].position).Length();
				open_list.Push(neighbor_idx, f_score);
			}
		}
	}

	// Backtrack path in reverse
	uint32_t path_len = 0;
	if (found) {
		uint32_t temp_path[MAX_PATH_LENGTH];
		uint32_t curr = end;
		while (curr != INVALID_GRAPH_NODE && path_len < MAX_PATH_LENGTH) {
			temp_path[path_len++] = curr;
			curr = ws.node_data[curr].parent;
		}
		for (uint32_t i = 0; i < path_len; ++i) {
			out_path[i] = temp_path[path_len - 1 - i];
		}
	}
	return path_len;
}

} // namespace ZHLN::ALife