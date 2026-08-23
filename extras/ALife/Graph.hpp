// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Vec3.h>
#include <cstddef>
#include <memory_resource>
#include <vector>

namespace ZHLN::ALife {

inline constexpr uint32_t MAX_NODE_NEIGHBORS = 4;

enum class NodeType : uint8_t { Wilderness, Hub, Campfire, Lair };

struct Node {
    JPH::RVec3 position;
    uint32_t   neighbors[MAX_NODE_NEIGHBORS];
    uint32_t   neighbor_count = 0;
    NodeType   type           = NodeType::Wilderness;
};

struct AStarData {
    float    g_score;
    uint32_t parent;
    bool     closed;
};

struct HeapNode {
    uint32_t node_idx;
    float    f_score;
};

struct PathWorkspace {
    std::pmr::vector<AStarData> node_data;
    std::pmr::vector<HeapNode>  heap_mem;

    PathWorkspace(uint32_t node_count, std::pmr::memory_resource* arena):
        node_data(node_count, arena), heap_mem(static_cast<size_t>(node_count * MAX_NODE_NEIGHBORS), arena) {
    }
};

class ZHLN_API LevelGraph {
  public:
    explicit LevelGraph(uint32_t node_count);

    void               Connect(uint32_t a, uint32_t b);
    [[nodiscard]] auto FindClosest(JPH::RVec3Arg pos) const -> uint32_t;

    // No locks required. Safe to call concurrently.
    auto FindPath(uint32_t start, uint32_t end, uint32_t* out_path, PathWorkspace& ws) const -> uint32_t;

    // Encapsulated accessors
    [[nodiscard]] auto GetNodes() const noexcept -> const std::vector<Node>& {
        return _nodes;
    }
    [[nodiscard]] auto GetNode(uint32_t index) -> Node& {
        return _nodes[index];
    }
    [[nodiscard]] auto GetNode(uint32_t index) const -> const Node& {
        return _nodes[index];
    }
    [[nodiscard]] auto GetNodeCount() const noexcept -> size_t {
        return _nodes.size();
    }

  private:
    std::vector<Node> _nodes;
};

} // namespace ZHLN::ALife
