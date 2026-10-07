// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace ZHLN {

class ZHLN_API TransformSystem {
  public:
    // Per-system cache for detecting local-transform and parent changes. The
    // transform components are intentionally still public/mutable, so the
    // system snapshots their values instead of relying on callers to remember
    // to set a dirty bit.
    struct ScratchStorage {
        static constexpr uint32_t NoNode = std::numeric_limits<uint32_t>::max();

        struct Snapshot {
            // Exact bit patterns for position, rotation and scale. This avoids
            // matrix construction just to find out whether a local transform
            // changed, while still handling every public-field mutation.
            std::array<uint32_t, 10> localBits {};
            // The last propagated output detects direct writes to public `world`
            // matrices, so a changed parent can invalidate its whole subtree.
            std::array<uint32_t, 16> worldBits {};
            Entity                   parent            = Entity::Null();
            uint64_t                 lastSeenEpoch     = 0;
            uint32_t                 generation        = 0;
            bool                     initialized       = false;
            bool                     active            = false;
            bool                     hasWorldTransform = false;
        };

        struct NodeIndex {
            uint32_t generation = 0;
            uint32_t node       = NoNode;
        };

        struct Node {
            Entity   entity        = Entity::Null();
            Entity   parent        = Entity::Null();
            uint32_t parentIndex   = NoNode;
            bool     dirty         = false;
            bool     preserveWorld = false;
        };

        std::vector<Snapshot>  snapshots;
        std::vector<NodeIndex> nodeIndices;
        std::vector<Node>      nodes;
        std::vector<uint32_t>  activeTransformIndices;
        std::vector<uint32_t>  nextActiveTransformIndices;
        std::vector<uint32_t>  childOffsets;
        std::vector<uint32_t>  childIndices;
        std::vector<uint32_t>  childCursor;
        std::vector<uint32_t>  traversalStack;
        std::vector<uint32_t>  cyclePath;
        std::vector<uint32_t>  updateRoots;
        std::vector<uint8_t>   visitColors;
        uint64_t               epoch = 0;
    };

    // A raw Registry& is explicit: a missing WorldTransformComponent must be
    // inserted, so the graph serialises this structural writer.
    static void Update(
        ECS::Query<const Components::HierarchyComponent, const Components::TransformComponent, Components::WorldTransformComponent&> query,
        ECS::Registry&                                                                                                               registry,
        ECS::Local<ScratchStorage>                                                                                                   scratch
    );

    // Called after Present. Copy only matrices whose world value differs from
    // previous, so direct world-matrix writers need no transient dirty flag.
    static void UpdateTransformHistory(ECS::Registry& registry) noexcept;

  private:
    [[nodiscard]] static auto LocalTransformBits(const Components::TransformComponent& transform) noexcept -> std::array<uint32_t, 10>;
    [[nodiscard]] static auto WorldMatrixBits(const JPH::Mat44& matrix) noexcept -> std::array<uint32_t, 16>;
    static void               BreakTransformCycles(ScratchStorage& scratch);
    static void               BuildChildren(ScratchStorage& scratch);
};

} // namespace ZHLN
