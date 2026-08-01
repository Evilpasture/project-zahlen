// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Zahlen/Components.hpp"
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/ecs/ECS.hpp>

namespace ZHLN {

class UILayoutSystem {
  public:
    struct UIViewport {
        float width;
        float height;
    };
    void ResolveLayouts(ECS::Registry& reg, const UIViewport& viewport) {
        auto entities = reg.GetEntitiesWith<Components::UIRectComponent>();
        auto rects    = reg.GetRawArray<Components::UIRectComponent>();

        if (entities.empty()) {
            return;
        }

        struct SortEntry {
            size_t   rawIndex;
            uint32_t depth;
        };
        JPH::Array<SortEntry> sortedEntries;
        sortedEntries.reserve(entities.size());

        for (size_t i = 0; i < entities.size(); ++i) {
            sortedEntries.push_back({.rawIndex = i, .depth = rects[i].hierarchyDepth});
        }

        std::ranges::sort(sortedEntries, [](const auto& a, const auto& b) { return a.depth < b.depth; });

        HashMap<uint64_t, float> stackOffsets;

        for (const auto& entry: sortedEntries) {
            Components::UIRectComponent& rect   = rects[entry.rawIndex];
            Entity                       parent = rect.parentEntity;

            float pMinX = 0.0f;
            float pMinY = 0.0f;
            float pMaxX = viewport.width;
            float pMaxY = viewport.height;

            if (parent != NullEntity && reg.IsAlive(parent)) {
                if (auto* pRect = reg.Get<Components::UIRectComponent>(parent)) {
                    pMinX = pRect->computedAbsMinX;
                    pMinY = pRect->computedAbsMinY;
                    pMaxX = pRect->computedAbsMaxX;
                    pMaxY = pRect->computedAbsMaxY;
                }
            }

            float pWidth  = pMaxX - pMinX;
            float pHeight = pMaxY - pMinY;

            if (parent != NullEntity && reg.IsAlive(parent)) {
                if (auto* stack = reg.Get<Components::UIStackComponent>(parent)) {
                    const float* offsetPtr     = stackOffsets.Find(parent.Pack());
                    float        currentOffset = (offsetPtr != nullptr) ? *offsetPtr : stack->padding;

                    if (stack->direction == StackDirection::Vertical) {
                        rect.y = currentOffset;
                    } else {
                        rect.x = currentOffset;
                    }
                }
            }

            float anchorLeft   = pMinX + (pWidth * rect.anchorMinX);
            float anchorRight  = pMinX + (pWidth * rect.anchorMaxX);
            float anchorTop    = pMinY + (pHeight * rect.anchorMinY);
            float anchorBottom = pMinY + (pHeight * rect.anchorMaxY);

            if (JPH::abs(rect.anchorMinX - rect.anchorMaxX) < 1e-5f) {
                rect.computedAbsMinX = anchorLeft + rect.x;
                rect.computedAbsMaxX = rect.computedAbsMinX + rect.width;
            } else {
                rect.computedAbsMinX = anchorLeft + rect.x;
                rect.computedAbsMaxX = anchorRight + rect.width;
            }

            if (JPH::abs(rect.anchorMinY - rect.anchorMaxY) < 1e-5f) {
                rect.computedAbsMinY = anchorTop + rect.y;
                rect.computedAbsMaxY = rect.computedAbsMinY + rect.height;
            } else {
                rect.computedAbsMinY = anchorTop + rect.y;
                rect.computedAbsMaxY = anchorBottom + rect.height;
            }

            if (parent != NullEntity && reg.IsAlive(parent)) {
                if (auto* stack = reg.Get<Components::UIStackComponent>(parent)) {
                    float nextOffset = 0.0f;
                    if (stack->direction == StackDirection::Vertical) {
                        float height = rect.computedAbsMaxY - rect.computedAbsMinY;
                        nextOffset   = rect.y + height + stack->spacing;
                    } else {
                        float width = rect.computedAbsMaxX - rect.computedAbsMinX;
                        nextOffset  = rect.x + width + stack->spacing;
                    }
                    stackOffsets.Insert(parent.Pack(), nextOffset);
                }
            }
        }
    }
};

} // namespace ZHLN
