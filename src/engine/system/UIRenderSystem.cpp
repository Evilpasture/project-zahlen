// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/system/UIRenderSystem.cpp
#include "UIRenderSystem.hpp"

#include "UILayoutSystem.hpp"

#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Window.hpp>
#include <algorithm>
#include <detail/HashMap.hpp>
#include <ecs/ECS.hpp>

namespace ZHLN {

void UIRenderSystem::Update(Engine& engine) {
	auto& reg = engine.GetRegistry();
	auto& rc = engine.GetRenderContext();
	auto windowSize = engine.GetWindow().GetSize();

	if (windowSize.width == 0 || windowSize.height == 0) {
		return;
	}

	// 1. Resolve Math Layouts
	UILayoutSystem layoutSystem;
	layoutSystem.ResolveLayouts(
		reg, {.width = (float)windowSize.width, .height = (float)windowSize.height});

	// 2. Gather and sort ALL UI elements by depth ascending for correct draw-order (Z-Index)
	auto entities = reg.GetEntitiesWith<Components::UIRectComponent>();
	auto rects = reg.GetRawArray<Components::UIRectComponent>();

	struct SortEntry {
		size_t rawIndex;
		uint32_t depth;
	};
	JPH::Array<SortEntry> sortedEntries;
	sortedEntries.reserve(entities.size());
	for (size_t i = 0; i < entities.size(); ++i) {
		sortedEntries.push_back({.rawIndex = i, .depth = rects[i].hierarchyDepth});
	}
	std::ranges::sort(sortedEntries, [](const auto& a, const auto& b) { return a.depth < b.depth; });

	HashMap<uint64_t, ScissorRect> activeScissors;

	for (const auto& entry : sortedEntries) {
		Entity e = entities[entry.rawIndex];
		const auto& rect = rects[entry.rawIndex];

		// A. Inherit scissor from parent if parent had scissoring active
		bool parentHasScissor = false;
		ScissorRect parentScissor{};

		if (rect.parentEntity != NullEntity && reg.IsAlive(rect.parentEntity)) {
			const ScissorRect* parentScissorPtr = activeScissors.Find(rect.parentEntity.Pack());
			if (parentScissorPtr != nullptr) {
				parentScissor = *parentScissorPtr;
				parentHasScissor = true;
			}
		}

		// B. Intersect active clipping regions
		bool useScissor = parentHasScissor;
		ScissorRect currentScissor = parentScissor;

		// Check if the parent of this element had `clipChildren = true`
		if (rect.parentEntity != NullEntity && reg.IsAlive(rect.parentEntity)) {
			if (auto* parentRect = reg.Get<Components::UIRectComponent>(rect.parentEntity)) {
				if (parentRect->clipChildren) {
					ScissorRect parentClip = {
						.x = (int32_t)std::max(0.0f, parentRect->computedAbsMinX),
						.y = (int32_t)std::max(0.0f, parentRect->computedAbsMinY),
						.width = (uint32_t)std::max(0.0f, parentRect->computedAbsMaxX -
															  parentRect->computedAbsMinX),
						.height = (uint32_t)std::max(0.0f, parentRect->computedAbsMaxY -
															   parentRect->computedAbsMinY)};

					if (!useScissor) {
						currentScissor = parentClip;
						useScissor = true;
					} else {
						// Intersect parent clipping bounds with ancestors' active boundaries
						int32_t x0 = std::max(currentScissor.x, parentClip.x);
						int32_t y0 = std::max(currentScissor.y, parentClip.y);
						int32_t x1 = std::min(currentScissor.x + (int32_t)currentScissor.width,
											  parentClip.x + (int32_t)parentClip.width);
						int32_t y1 = std::min(currentScissor.y + (int32_t)currentScissor.height,
											  parentClip.y + (int32_t)parentClip.height);

						currentScissor.x = x0;
						currentScissor.y = y0;
						currentScissor.width = std::max(0, x1 - x0);
						currentScissor.height = std::max(0, y1 - y0);
					}
				}
			}
		}

		// Cache current scissor for nested children down the line
		if (useScissor) {
			activeScissors.Insert(e.Pack(), currentScissor);
		}

		// C. Draw Panel
		if (auto* panel = reg.Get<Components::UIPanelComponent>(e)) {
			if (panel->isDirty || panel->mesh.posBuffer == BufferHandle::Invalid) {
				if (panel->mesh.posBuffer != BufferHandle::Invalid) {
					rc.DestroyBuffer(panel->mesh.posBuffer);
					rc.DestroyBuffer(panel->mesh.attrBuffer);
				}
				panel->mesh = GUI::CreatePanelMesh(rc, rect, *panel);
				panel->isDirty = false;
			}
			Renderer::DrawUI(rc, panel->mesh, panel->textureIndex, useScissor, currentScissor);
		}
	}

	// 3. Process Text & Text Input Components
	auto uiSettingsEntities = reg.GetEntitiesWith<Components::UISettingsComponent>();
	const FontAtlas* activeFont = nullptr;
	if (!uiSettingsEntities.empty()) {
		activeFont = &reg.Get<Components::UISettingsComponent>(uiSettingsEntities[0])->fontAtlas;
	}

	for (Entity e : reg.GetEntitiesWith<Components::TextComponent>()) {
		auto* text = reg.Get<Components::TextComponent>(e);
		float drawX = text->x;
		float drawY = text->y;
		bool hasRect = false;

		auto* rect = reg.Get<Components::UIRectComponent>(e);
		if (rect != nullptr) {
			drawX = rect->computedAbsMinX + text->x;
			drawY = rect->computedAbsMinY + text->y;
			hasRect = true;
		}

		// Handle raw text input synchronization and cursor appending
		if (auto* input = reg.Get<Components::UITextInputComponent>(e)) {
			std::string_view raw = input->text;
			std::string displayStr;

			if (input->isFocused) {
				// Inject a vertical bar cursor '|' at the current cursor index position
				displayStr = std::string(raw.substr(0, input->cursorIndex)) + "|" +
							 std::string(raw.substr(input->cursorIndex));
			} else {
				displayStr = raw;
			}

			if (displayStr != text->text.c_str()) {
				if (text->mesh.posBuffer != BufferHandle::Invalid) {
					rc.DestroyBuffer(text->mesh.posBuffer);
					rc.DestroyBuffer(text->mesh.attrBuffer);
				}
				text->text.assign(displayStr);
				text->mesh.posBuffer = BufferHandle::Invalid;
				text->mesh.attrBuffer = BufferHandle::Invalid;
			}
		}

		// If the computed absolute coordinates changed (due to dragging), rebuild the text mesh
		if (text->lastDrawX != drawX || text->lastDrawY != drawY) {
			if (text->mesh.posBuffer != BufferHandle::Invalid) {
				rc.DestroyBuffer(text->mesh.posBuffer);
				rc.DestroyBuffer(text->mesh.attrBuffer);
				text->mesh.posBuffer = BufferHandle::Invalid;
				text->mesh.attrBuffer = BufferHandle::Invalid;
			}
			text->lastDrawX = drawX;
			text->lastDrawY = drawY;
		}

		if (text->mesh.posBuffer == BufferHandle::Invalid && activeFont != nullptr) {
			text->mesh = GUI::CreateTextMesh(rc, *activeFont, text->text.c_str(), drawX, drawY,
											 text->scale, text->color);
		}

		// Calculate scissor constraints (using O(1) hash lookups from step 2)
		bool useScissor = false;
		ScissorRect currentScissor{};

		if (hasRect) {
			const ScissorRect* parentScissorPtr = activeScissors.Find(rect->parentEntity.Pack());
			if (parentScissorPtr != nullptr) {
				currentScissor = *parentScissorPtr;
				useScissor = true;
			} else {
				// Fallback: Check if the direct parent itself had clipping enabled
				if (rect->parentEntity != NullEntity && reg.IsAlive(rect->parentEntity)) {
					if (auto* parentRect =
							reg.Get<Components::UIRectComponent>(rect->parentEntity)) {
						if (parentRect->clipChildren) {
							currentScissor = {
								.x = (int32_t)std::max(0.0f, parentRect->computedAbsMinX),
								.y = (int32_t)std::max(0.0f, parentRect->computedAbsMinY),
								.width = (uint32_t)std::max(0.0f, parentRect->computedAbsMaxX -
																	  parentRect->computedAbsMinX),
								.height = (uint32_t)std::max(0.0f, parentRect->computedAbsMaxY -
																	   parentRect->computedAbsMinY)};
							useScissor = true;
						}
					}
				}
			}
		}

		Renderer::DrawUI(rc, text->mesh, text->fontIndex, useScissor, currentScissor);
	}
}

} // namespace ZHLN
