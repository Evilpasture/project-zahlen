#pragma once

#include "Types.hpp"

#include <memory_resource>
#include <vector>

namespace ZHLN::ALife {

constexpr uint32_t MAX_NODE_NEIGHBORS = 4;

enum class NodeType : uint8_t { Wilderness, Hub, Campfire, Lair };

struct Node {
	JPH::RVec3 position;
	uint32_t neighbors[MAX_NODE_NEIGHBORS];
	uint32_t neighbor_count = 0;
	NodeType type = NodeType::Wilderness;
};

struct AStarData {
	float g_score;
	uint32_t parent;
	bool closed;
};

struct HeapNode {
	uint32_t node_idx;
	float f_score;
};

struct PathWorkspace {
	std::pmr::vector<AStarData> node_data;
	std::pmr::vector<HeapNode> heap_mem;

	PathWorkspace(uint32_t node_count, std::pmr::memory_resource* arena)
		: node_data(node_count, arena), heap_mem(node_count * MAX_NODE_NEIGHBORS, arena) {}
};

class LevelGraph {
  public:
	explicit LevelGraph(uint32_t node_count);

	void Connect(uint32_t a, uint32_t b);
	[[nodiscard]] uint32_t FindClosest(JPH::RVec3Arg pos) const;

	// No locks required. Safe to call concurrently.
	uint32_t FindPath(uint32_t start, uint32_t end, uint32_t* out_path, PathWorkspace& ws) const;

	// Encapsulated accessors
	[[nodiscard]] const std::vector<Node>& GetNodes() const noexcept { return _nodes; }
	[[nodiscard]] Node& GetNode(uint32_t index) { return _nodes[index]; }
	[[nodiscard]] const Node& GetNode(uint32_t index) const { return _nodes[index]; }
	[[nodiscard]] size_t GetNodeCount() const noexcept { return _nodes.size(); }

  private:
	std::vector<Node> _nodes;
};

} // namespace ZHLN::ALife