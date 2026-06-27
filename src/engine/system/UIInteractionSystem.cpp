#include "UIInteractionSystem.hpp"

#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Input.hpp>
#include <algorithm>
#include <ecs/ECS.hpp>

namespace ZHLN {

void UIInteractionSystem::Update(Engine& engine) {
	auto& reg = engine.GetRegistry();
	auto& input = engine.GetInput();
	auto mouse = input.GetMouse();

	auto entities = reg.GetEntitiesWith<UIRectComponent>();
	auto rects = reg.GetRawArray<UIRectComponent>();

	if (entities.empty()) {
		return;
	}

	bool leftMouseDown = input.IsMouseButtonDown(KeyCode::LButton);

	for (Entity e : reg.GetEntitiesWith<UIDragComponent>()) {
		auto* drag = reg.Get<UIDragComponent>(e);
		if (drag->isDragging) {
			if (!leftMouseDown) {
				drag->isDragging = false;
			} else {
				// Move the target master panel by the raw mouse delta
				if (auto* targetRect = reg.Get<UIRectComponent>(drag->targetEntity)) {
					targetRect->x += mouse.deltaX;
					targetRect->y += mouse.deltaY;
				}
			}
		}
	}

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
					  [](const SortEntry& a, const SortEntry& b) { return a.depth > b.depth; });

	bool clickConsumed = false;

	for (const auto& entry : sortedEntries) {
		Entity e = entities[entry.rawIndex];
		const auto& rect = rects[entry.rawIndex];
		auto* button = reg.Get<UIButtonComponent>(e);

		if (button == nullptr) {
			continue;
		}

		// Clear the click flag for the new frame
		button->Set(UIButton::Clicked, false);

		if (clickConsumed) {
			button->Set(UIButton::Hovered, false);
			button->Set(UIButton::Pressed, false);
			continue;
		}

		bool inside = (mouse.x >= rect.computedAbsMinX && mouse.x <= rect.computedAbsMaxX &&
					   mouse.y >= rect.computedAbsMinY && mouse.y <= rect.computedAbsMaxY);

		if (inside) {
			button->Set(UIButton::Hovered, true);
			if (leftMouseDown) {
				button->Set(UIButton::Pressed, true);
				// If this element has a drag component, initiate dragging
				if (auto* drag = reg.Get<UIDragComponent>(e)) {
					drag->isDragging = true;
				}
			} else {
				if (button->Has(UIButton::Pressed)) {
					button->Set(UIButton::Clicked, true);
					clickConsumed = true; // Consume the event
				}
				button->Set(UIButton::Pressed, false);
			}
		} else {
			button->Set(UIButton::Hovered, false);
			if (!leftMouseDown) {
				button->Set(UIButton::Pressed, false);
			}
		}
	}
}

} // namespace ZHLN
