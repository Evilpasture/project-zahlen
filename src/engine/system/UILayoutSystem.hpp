// src/engine/system/UILayoutSystem.hpp
#pragma once

#include "Zahlen/Components.hpp"
#include "ecs/ECS.hpp"

namespace ZHLN {

class UILayoutSystem {
  public:
	struct UIViewport {
		float width;
		float height;
	};
	void ResolveLayouts(ECS::Registry& reg, const UIViewport& viewport) {
		auto entities = reg.GetEntitiesWith<UIRectComponent>();
		auto rects = reg.GetRawArray<UIRectComponent>();

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

		// 2. Solve layouts linearly
		for (const auto& entry : sortedEntries) {
			Entity e = entities[entry.rawIndex]; // Defined here
			UIRectComponent& rect = rects[entry.rawIndex];
			Entity parent = rect.parentEntity;

			// Resolve parent bounds (default to viewport if root)
			float pMinX = 0.0f;
			float pMinY = 0.0f;
			float pMaxX = viewport.width;
			float pMaxY = viewport.height;

			if (parent != NullEntity && reg.IsAlive(parent)) {
				if (auto* pRect = reg.Get<UIRectComponent>(parent)) {
					pMinX = pRect->computedAbsMinX;
					pMinY = pRect->computedAbsMinY;
					pMaxX = pRect->computedAbsMaxX;
					pMaxY = pRect->computedAbsMaxY;
				}
			}

			float pWidth = pMaxX - pMinX;
			float pHeight = pMaxY - pMinY;

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
				// Fixed Width: x is offset relative to anchor, width is absolute
				rect.computedAbsMinX = anchorLeft + rect.x;
				rect.computedAbsMaxX = rect.computedAbsMinX + rect.width;
			} else {
				// Stretching: x acts as left margin, width acts as right margin (usually negative)
				rect.computedAbsMinX = anchorLeft + rect.x;
				rect.computedAbsMaxX = anchorRight + rect.width;
			}

			// Resolve vertical positioning
			if (JPH::abs(rect.anchorMinY - rect.anchorMaxY) < 1e-5f) {
				// Fixed Height
				rect.computedAbsMinY = anchorTop + rect.y;
				rect.computedAbsMaxY = rect.computedAbsMinY + rect.height;
			} else {
				// Stretching
				rect.computedAbsMinY = anchorTop + rect.y;
				rect.computedAbsMaxY = anchorBottom + rect.height;
			}

			// If absolute position changed, mark the panel dirty to force a mesh rebuild
			if (rect.computedAbsMinX != oldMinX || rect.computedAbsMinY != oldMinY ||
				rect.computedAbsMaxX != oldMaxX || rect.computedAbsMaxY != oldMaxY) {
				if (auto* panel = reg.Get<UIPanelComponent>(e)) {
					panel->isDirty = true;
				}
			}
		}
	}
};

} // namespace ZHLN
