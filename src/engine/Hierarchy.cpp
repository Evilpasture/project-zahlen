// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Hierarchy.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace ZHLN {

void MarkPendingDestroy(ECS::Registry& registry, Entity root) {
    if (registry.IsAlive(root) && registry.Get<Components::PendingDestroy>(root) == nullptr) {
        registry.Add(root, Components::PendingDestroy {});
    }
}

void ExpandPendingDestroy(ECS::Registry& registry) {
    const auto marked = registry.GetEntitiesWith<Components::PendingDestroy>();
    if (marked.empty()) {
        return;
    }

    struct Link {
        uint64_t parent;
        Entity child;
    };
    std::vector<Link> children;
    const auto hierarchyEntities = registry.GetEntitiesWith<Components::HierarchyComponent>();
    if (!hierarchyEntities.empty()) {
        const auto hierarchy = registry.GetRawArray<Components::HierarchyComponent>();
        children.reserve(hierarchyEntities.size());
        for (size_t i = 0; i < hierarchyEntities.size(); ++i) {
            children.push_back({hierarchy[i].parent.Pack(), hierarchyEntities[i]});
        }
        std::sort(children.begin(), children.end(), [](const Link& a, const Link& b) { return a.parent < b.parent; });
    }

    // Adding tags may resize the PendingDestroy sparse set; work on a copy.
    std::vector<Entity> frontier(marked.begin(), marked.end());
    while (!frontier.empty()) {
        const Entity parent = frontier.back();
        frontier.pop_back();
        const auto first = std::lower_bound(children.begin(), children.end(), parent.Pack(),
                                            [](const Link& link, uint64_t key) { return link.parent < key; });
        for (auto it = first; it != children.end() && it->parent == parent.Pack(); ++it) {
            if (registry.IsAlive(it->child) && registry.Get<Components::PendingDestroy>(it->child) == nullptr) {
                registry.Add(it->child, Components::PendingDestroy {});
                frontier.push_back(it->child);
            }
        }
    }
}

void DespawnEntity(Engine& engine, Entity entity) {
    auto& registry = engine.GetRegistry();
    if (!registry.IsAlive(entity)) {
        return;
    }
    MarkPendingDestroy(registry, entity);
    ExpandPendingDestroy(registry);
}

} // namespace ZHLN
