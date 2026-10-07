// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TransformSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ranges>

namespace ZHLN {
namespace {

using TransformQuery = ECS::Query<const Components::HierarchyComponent, const Components::TransformComponent, Components::WorldTransformComponent&>;
using ScratchStorage = TransformSystem::ScratchStorage;

// A transformless hierarchy node contributes an identity local transform, so
// it can be skipped while looking for the closest ancestor with a Transform.
// The hop bound also makes malformed transformless cycles harmless.
[[nodiscard]] auto FindNearestTransformAncestor(ECS::Registry& registry, Entity entity, size_t maxHierarchyHops) noexcept -> Entity {
    const auto hierarchy = registry.Get<Components::HierarchyComponent>(entity);
    Entity     parent    = hierarchy ? hierarchy->parent : Entity::Null();

    for (size_t hop = 0; parent != Entity::Null() && hop <= maxHierarchyHops; ++hop) {
        if (!registry.IsAlive(parent)) {
            return Entity::Null();
        }
        if (registry.Get<Components::TransformComponent>(parent)) {
            return parent;
        }
        const auto parentHierarchy = registry.Get<Components::HierarchyComponent>(parent);
        if (!parentHierarchy) {
            return Entity::Null();
        }
        parent = parentHierarchy->parent;
    }
    return Entity::Null();
}

} // namespace

auto TransformSystem::LocalTransformBits(const Components::TransformComponent& transform) noexcept -> std::array<uint32_t, 10> {
    return {
        std::bit_cast<uint32_t>(transform.position.GetX()), std::bit_cast<uint32_t>(transform.position.GetY()),
        std::bit_cast<uint32_t>(transform.position.GetZ()), std::bit_cast<uint32_t>(transform.rotation.GetX()),
        std::bit_cast<uint32_t>(transform.rotation.GetY()), std::bit_cast<uint32_t>(transform.rotation.GetZ()),
        std::bit_cast<uint32_t>(transform.rotation.GetW()), std::bit_cast<uint32_t>(transform.scale.GetX()),
        std::bit_cast<uint32_t>(transform.scale.GetY()),    std::bit_cast<uint32_t>(transform.scale.GetZ()),
    };
}

auto TransformSystem::WorldMatrixBits(const JPH::Mat44& matrix) noexcept -> std::array<uint32_t, 16> {
    std::array<uint32_t, 16> bits {};
    for (uint32_t column = 0; column < 4; ++column) {
        const JPH::Vec4 value = matrix.GetColumn4(column);
        const size_t    base  = static_cast<size_t>(column) * 4;
        bits[base + 0]        = std::bit_cast<uint32_t>(value.GetX());
        bits[base + 1]        = std::bit_cast<uint32_t>(value.GetY());
        bits[base + 2]        = std::bit_cast<uint32_t>(value.GetZ());
        bits[base + 3]        = std::bit_cast<uint32_t>(value.GetW());
    }
    return bits;
}

void TransformSystem::BreakTransformCycles(ScratchStorage& scratch) {
    const uint32_t nodeCount = static_cast<uint32_t>(scratch.nodes.size());
    scratch.visitColors.assign(nodeCount, 0);
    scratch.cyclePath.clear();

    for (uint32_t start = 0; start < nodeCount; ++start) {
        if (scratch.visitColors[start] != 0) {
            continue;
        }

        scratch.cyclePath.clear();
        uint32_t current = start;
        while (current != ScratchStorage::NoNode && scratch.visitColors[current] == 0) {
            scratch.visitColors[current] = 1;
            scratch.cyclePath.push_back(current);
            current = scratch.nodes[current].parentIndex;
        }

        if (current != ScratchStorage::NoNode && scratch.visitColors[current] == 1) {
            // Malformed parent loops used to be bounded by a recursion-depth
            // fallback. Cut one deterministic edge instead, so the iterative
            // propagation always terminates and the rest of the rig stays usable.
            const auto cycleStart = std::ranges::find(scratch.cyclePath, current);
            if (cycleStart != scratch.cyclePath.end()) {
                uint32_t cutNode = *cycleStart;
                for (auto it = cycleStart; it != scratch.cyclePath.end(); ++it) {
                    if (scratch.nodes[*it].entity.Pack() < scratch.nodes[cutNode].entity.Pack()) {
                        cutNode = *it;
                    }
                }
                scratch.nodes[cutNode].parentIndex = ScratchStorage::NoNode;
                scratch.nodes[cutNode].dirty       = true;
            }
        }

        for (uint32_t index: scratch.cyclePath) {
            scratch.visitColors[index] = 2;
        }
    }
}

void TransformSystem::BuildChildren(ScratchStorage& scratch) {
    const uint32_t nodeCount = static_cast<uint32_t>(scratch.nodes.size());
    scratch.childOffsets.assign(static_cast<size_t>(nodeCount) + 1, 0);

    for (const auto& node: scratch.nodes) {
        if (node.parentIndex != ScratchStorage::NoNode) {
            ++scratch.childOffsets[node.parentIndex + 1];
        }
    }
    for (uint32_t index = 1; index <= nodeCount; ++index) {
        scratch.childOffsets[index] += scratch.childOffsets[index - 1];
    }

    scratch.childIndices.resize(scratch.childOffsets.back());
    scratch.childCursor.assign(scratch.childOffsets.begin(), scratch.childOffsets.end() - 1);
    for (uint32_t childIndex = 0; childIndex < nodeCount; ++childIndex) {
        const uint32_t parentIndex = scratch.nodes[childIndex].parentIndex;
        if (parentIndex != ScratchStorage::NoNode) {
            scratch.childIndices[scratch.childCursor[parentIndex]++] = childIndex;
        }
    }
}

void TransformSystem::Update(TransformQuery query, ECS::Registry& registry, ECS::Local<ScratchStorage> scratch) {
    auto&      state             = *scratch;
    const auto entities          = query.Entities<Components::TransformComponent>();
    const auto hierarchyEntities = registry.GetEntitiesWith<Components::HierarchyComponent>();

    if (++state.epoch == 0) {
        for (auto& snapshot: state.snapshots) {
            snapshot.lastSeenEpoch = 0;
        }
        state.epoch = 1;
    }

    state.nodes.clear();
    state.nextActiveTransformIndices.clear();
    state.nodes.reserve(entities.size());
    state.nextActiveTransformIndices.reserve(entities.size());

    uint32_t maxEntityIndex = 0;
    bool     hasEntities    = false;
    for (Entity entity: entities) {
        maxEntityIndex = std::max(maxEntityIndex, entity.index);
        hasEntities    = true;
    }
    if (hasEntities) {
        const size_t required = static_cast<size_t>(maxEntityIndex) + 1;
        if (state.snapshots.size() < required) {
            state.snapshots.resize(required);
        }
        if (state.nodeIndices.size() < required) {
            state.nodeIndices.resize(required);
        }
    }

    // First index every Transform entity. This lets children resolve their
    // nearest Transform ancestor in O(1), even when Registry's dense component
    // order is unrelated to hierarchy order.
    for (Entity entity: entities) {
        const uint32_t nodeIndex = static_cast<uint32_t>(state.nodes.size());
        state.nodes.push_back(ScratchStorage::Node {.entity = entity});
        state.nodeIndices[entity.index] = ScratchStorage::NodeIndex {.generation = entity.generation, .node = nodeIndex};
        state.nextActiveTransformIndices.push_back(entity.index);
    }

    const size_t maxHierarchyHops = hierarchyEntities.size() + 1;
    for (auto& node: state.nodes) {
        node.parent = FindNearestTransformAncestor(registry, node.entity, maxHierarchyHops);
        if (node.parent != Entity::Null() && node.parent.index < state.nodeIndices.size()) {
            const ScratchStorage::NodeIndex parentSlot = state.nodeIndices[node.parent.index];
            if (parentSlot.generation == node.parent.generation && parentSlot.node != ScratchStorage::NoNode) {
                node.parentIndex = parentSlot.node;
            } else {
                // A live Transform ancestor must have been indexed above. If it
                // was not, treat it as a root rather than retaining a bad index.
                node.parent = Entity::Null();
            }
        }

        const auto transform = registry.Get<Components::TransformComponent>(node.entity);
        if (!transform) {
            continue;
        }

        ScratchStorage::Snapshot& snapshot  = state.snapshots[node.entity.index];
        const bool                wasActive = snapshot.initialized && snapshot.active && snapshot.generation == node.entity.generation;
        const bool                isNew     = !wasActive;
        const auto                localBits = LocalTransformBits(*transform);
        const auto                world     = registry.Get<Components::WorldTransformComponent>(node.entity);
        const bool                hasWorld  = static_cast<bool>(world);
        const bool inputDirty      = isNew || snapshot.localBits != localBits || snapshot.parent != node.parent || snapshot.hasWorldTransform != hasWorld ||
                                     !hasWorld;
        const bool worldWasWritten = wasActive && snapshot.hasWorldTransform && hasWorld && snapshot.worldBits != WorldMatrixBits(world->world);

        // A world-only write is authoritative for this node, but still invalidates
        // its descendants. Local TRS, parent, and component-lifecycle changes keep
        // the usual rule: recompute this node from its inputs.
        node.preserveWorld = worldWasWritten && !inputDirty;
        node.dirty         = inputDirty || worldWasWritten;

        snapshot.localBits         = localBits;
        snapshot.parent            = node.parent;
        snapshot.generation        = node.entity.generation;
        snapshot.lastSeenEpoch     = state.epoch;
        snapshot.initialized       = true;
        snapshot.active            = true;
        snapshot.hasWorldTransform = hasWorld;
    }

    // Remove snapshots for transforms removed since the previous update. The
    // cache is indexed by entity id, so this sweep is bounded by the currently
    // allocated entity-index range and never leaves despawned generations live.
    for (uint32_t entityIndex: state.activeTransformIndices) {
        if (entityIndex >= state.snapshots.size()) {
            continue;
        }
        auto& snapshot = state.snapshots[entityIndex];
        if (snapshot.active && snapshot.lastSeenEpoch != state.epoch) {
            snapshot.active      = false;
            snapshot.initialized = false;
            snapshot.parent      = Entity::Null();
        }
    }
    state.activeTransformIndices.swap(state.nextActiveTransformIndices);

    BreakTransformCycles(state);
    BuildChildren(state);

    state.updateRoots.clear();
    for (uint32_t index = 0; index < state.nodes.size(); ++index) {
        const auto& node = state.nodes[index];
        if (!node.dirty) {
            continue;
        }
        if (node.parentIndex == ScratchStorage::NoNode || !state.nodes[node.parentIndex].dirty) {
            state.updateRoots.push_back(index);
        }
    }

    for (uint32_t root: state.updateRoots) {
        state.traversalStack.clear();
        state.traversalStack.push_back(root);

        while (!state.traversalStack.empty()) {
            const uint32_t index = state.traversalStack.back();
            state.traversalStack.pop_back();
            const auto& node = state.nodes[index];

            const auto transform = registry.Get<Components::TransformComponent>(node.entity);
            if (!transform) {
                continue;
            }

            JPH::Mat44 parentWorld = JPH::Mat44::sIdentity();
            if (node.parentIndex != ScratchStorage::NoNode) {
                const Entity parentEntity = state.nodes[node.parentIndex].entity;
                if (const auto parentWorldTransform = registry.Get<Components::WorldTransformComponent>(parentEntity)) {
                    parentWorld = parentWorldTransform->world;
                }
            }

            const JPH::Mat44 computedWorld = parentWorld * transform->GetLocalMatrix();
            auto&            snapshot      = state.snapshots[node.entity.index];
            if (auto worldTransform = registry.Get<Components::WorldTransformComponent>(node.entity)) {
                if (!node.preserveWorld && !Math::SameMatrix(worldTransform->world, computedWorld)) {
                    worldTransform->world = computedWorld;
                }
                snapshot.worldBits = WorldMatrixBits(worldTransform->world);
            } else {
                auto& insertedWorldTransform    = registry.Add<Components::WorldTransformComponent>(node.entity);
                insertedWorldTransform.world    = computedWorld;
                insertedWorldTransform.previous = computedWorld;
                snapshot.hasWorldTransform      = true;
                snapshot.worldBits              = WorldMatrixBits(computedWorld);
            }

            const uint32_t childBegin = state.childOffsets[index];
            const uint32_t childEnd   = state.childOffsets[index + 1];
            for (uint32_t child = childBegin; child < childEnd; ++child) {
                state.traversalStack.push_back(state.childIndices[child]);
            }
        }
    }
}

void TransformSystem::UpdateTransformHistory(ECS::Registry& registry) noexcept {
    const auto entities        = registry.GetEntitiesWith<Components::WorldTransformComponent>();
    auto       worldTransforms = registry.GetRawArray<Components::WorldTransformComponent>();

    // WorldTransformComponent is public output data, and existing systems may
    // write `world` directly (including on entities without TransformComponent).
    // Compare against the last rendered matrix here rather than relying on every
    // writer to set a transient component flag. Static transforms incur reads
    // but no redundant previous-matrix writes.
    for (size_t i = 0; i < entities.size(); ++i) {
        if (!Math::SameMatrix(worldTransforms[i].world, worldTransforms[i].previous)) {
            worldTransforms[i].previous = worldTransforms[i].world;
        }
    }
}

} // namespace ZHLN
