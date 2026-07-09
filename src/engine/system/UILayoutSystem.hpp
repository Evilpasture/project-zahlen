// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/system/UILayoutSystem.hpp
#pragma once

#include "Zahlen/Components.hpp"
#include "detail/HashMap.hpp"
#include "ecs/ECS.hpp"

namespace ZHLN {

class UILayoutSystem {
  public:
	struct UIViewport {
		float width;
		float height;
	};
	void ResolveLayouts(ECS::Registry& reg, const UIViewport& viewport) {
		auto entities = reg.GetEntitiesWith<Components::UIRectComponent>();
		auto rects = reg.GetRawArray<Components::UIRectComponent>();

		if (entities.empty()) {
			return;
		}

		// 1. Gather and sort indices linearly by hierarchy depth
		struct SortEntry {
			size_t rawIndex;
			uint32_t depth;
		};
		JPH::Array<SortEntry> sortedEntries;
		sortedEntries.reserve(entities.size());

		for (size_t i = 0; i < entities.size(); ++i) {
			sortedEntries.push_back({.rawIndex = i, .depth = rects[i].hierarchyDepth});
		}

		std::ranges::sort(sortedEntries,
						  [](const auto& a, const auto& b) { return a.depth < b.depth; });

		// Fast, allocation-free offset accumulator map
		HashMap<uint64_t, float> stackOffsets;

		// 2. Solve layouts linearly
		for (const auto& entry : sortedEntries) {
			Entity e = entities[entry.rawIndex];
			Components::UIRectComponent& rect = rects[entry.rawIndex];
			Entity parent = rect.parentEntity;

			// Resolve parent bounds (default to viewport if root)
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

			float pWidth = pMaxX - pMinX;
			float pHeight = pMaxY - pMinY;

			// --- STACK CONTAINER POSITION OVERRIDE ---
			if (parent != NullEntity && reg.IsAlive(parent)) {
				if (auto* stack = reg.Get<Components::UIStackComponent>(parent)) {
					const float* offsetPtr = stackOffsets.Find(parent.Pack());
					float currentOffset = (offsetPtr != nullptr) ? *offsetPtr : stack->padding;

					if (stack->direction == StackDirection::Vertical) {
						rect.y = currentOffset; // Override vertical coordinate
					} else {
						rect.x = currentOffset; // Override horizontal coordinate
					}
				}
			}

			// Calculate anchor reference points in parent space
			float anchorLeft = pMinX + (pWidth * rect.anchorMinX);
			float anchorRight = pMinX + (pWidth * rect.anchorMaxX);
			float anchorTop = pMinY + (pHeight * rect.anchorMinY);
			float anchorBottom = pMinY + (pHeight * rect.anchorMaxY);

			// Cache previous absolute coordinates
			float oldMinX = rect.computedAbsMinX;
			float oldMinY = rect.computedAbsMinY;
			float oldMaxX = rect.computedAbsMaxX;
			float oldMaxY = rect.computedAbsMaxY;

			// Resolve horizontal positioning
			if (JPH::abs(rect.anchorMinX - rect.anchorMaxX) < 1e-5f) {
				rect.computedAbsMinX = anchorLeft + rect.x;
				rect.computedAbsMaxX = rect.computedAbsMinX + rect.width;
			} else {
				rect.computedAbsMinX = anchorLeft + rect.x;
				rect.computedAbsMaxX = anchorRight + rect.width;
			}

			// Resolve vertical positioning
			if (JPH::abs(rect.anchorMinY - rect.anchorMaxY) < 1e-5f) {
				rect.computedAbsMinY = anchorTop + rect.y;
				rect.computedAbsMaxY = rect.computedAbsMinY + rect.height;
			} else {
				rect.computedAbsMinY = anchorTop + rect.y;
				rect.computedAbsMaxY = anchorBottom + rect.height;
			}

			// --- UPDATE STACK ACCUMULATOR OFFSET ---
			if (parent != NullEntity && reg.IsAlive(parent)) {
				if (auto* stack = reg.Get<Components::UIStackComponent>(parent)) {
					float nextOffset = 0.0f;
					if (stack->direction == StackDirection::Vertical) {
						float height = rect.computedAbsMaxY - rect.computedAbsMinY;
						nextOffset = rect.y + height + stack->spacing;
					} else {
						float width = rect.computedAbsMaxX - rect.computedAbsMinX;
						nextOffset = rect.x + width + stack->spacing;
					}
					stackOffsets.Insert(parent.Pack(), nextOffset);
				}
			}

			// If absolute position changed, mark the panel dirty to force a mesh rebuild
			if (rect.computedAbsMinX != oldMinX || rect.computedAbsMinY != oldMinY ||
				rect.computedAbsMaxX != oldMaxX || rect.computedAbsMaxY != oldMaxY) {
				if (auto* panel = reg.Get<Components::UIPanelComponent>(e)) {
					panel->isDirty = true;
				}
			}
		}
	}
};

} // namespace ZHLN
