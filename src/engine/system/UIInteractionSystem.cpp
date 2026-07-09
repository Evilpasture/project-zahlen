// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

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

	auto entities = reg.GetEntitiesWith<Components::UIRectComponent>();
	auto rects = reg.GetRawArray<Components::UIRectComponent>();

	if (entities.empty()) {
		return;
	}

	bool leftMouseDown = input.IsMouseButtonDown(KeyCode::LButton);

	// 1. Process active dragging
	for (Entity e : reg.GetEntitiesWith<Components::UIDragComponent>()) {
		auto* drag = reg.Get<Components::UIDragComponent>(e);
		if (drag->isDragging) {
			if (!leftMouseDown) {
				drag->isDragging = false;
			} else {
				if (auto* targetRect = reg.Get<Components::UIRectComponent>(drag->targetEntity)) {
					targetRect->x += mouse.deltaX;
					targetRect->y += mouse.deltaY;
				}
			}
		}
	}

	// 2. Sort elements by depth descending to handle overlaps (Deeper children processed first)
	struct SortEntry {
		size_t rawIndex;
		uint32_t depth;
	};
	JPH::Array<SortEntry> sortedEntries;
	sortedEntries.reserve(entities.size());
	for (size_t i = 0; i < entities.size(); ++i) {
		sortedEntries.push_back({.rawIndex = i, .depth = rects[i].hierarchyDepth});
	}
	std::ranges::sort(sortedEntries, [](const auto& a, const auto& b) { return a.depth > b.depth; });

	bool clickConsumed = false;
	bool focusCaptured = false;

	for (const auto& entry : sortedEntries) {
		Entity e = entities[entry.rawIndex];
		const auto& rect = rects[entry.rawIndex];
		auto* button = reg.Get<Components::UIButtonComponent>(e);

		if (button == nullptr) {
			continue;
		}

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

				// Handle drag start
				if (auto* drag = reg.Get<Components::UIDragComponent>(e)) {
					drag->isDragging = true;
				}

				// Handle focus capture
				if (reg.Get<Components::UITextInputComponent>(e) != nullptr) {
					focusCaptured = true;
					for (Entity other : reg.GetEntitiesWith<Components::UITextInputComponent>()) {
						if (auto* inputComp = reg.Get<Components::UITextInputComponent>(other)) {
							inputComp->isFocused = (other == e);
						}
					}
				}
			} else {
				if (button->Has(UIButton::Pressed)) {
					button->Set(UIButton::Clicked, true);
					clickConsumed = true;
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

	// If clicked blank space outside any text fields, clear active focus
	if (leftMouseDown && !focusCaptured) {
		for (Entity e : reg.GetEntitiesWith<Components::UITextInputComponent>()) {
			if (auto* inputComp = reg.Get<Components::UITextInputComponent>(e)) {
				inputComp->isFocused = false;
			}
		}
	}
}

} // namespace ZHLN
