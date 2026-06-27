// src/engine/system/UIRenderSystem.cpp
#include "UIRenderSystem.hpp"

#include "UILayoutSystem.hpp"

#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Window.hpp>
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

	// 2. Build Geometry & Submit
	for (Entity e : reg.GetEntitiesWith<UIPanelComponent>()) {
		auto* rect = reg.Get<UIRectComponent>(e);
		auto* panel = reg.Get<UIPanelComponent>(e);

		if (rect == nullptr) {
			continue;
		}

		// Note: For a production UI, you'd want to check if rect bounds actually changed
		// to toggle isDirty, but for now we'll just check the flag or missing mesh.
		if (panel->isDirty || panel->mesh.posBuffer == BufferHandle::Invalid) {

			// Clean up old Vulkan buffers to prevent memory leaks!
			if (panel->mesh.posBuffer != BufferHandle::Invalid) {
				rc.DestroyBuffer(panel->mesh.posBuffer);
				rc.DestroyBuffer(panel->mesh.attrBuffer);
			}

			panel->mesh = GUI::CreatePanelMesh(rc, *rect, *panel);
			panel->isDirty = false;
		}

		// Submit to the UI pass. (We repurpose fontIndex to pass the Bindless Texture Index)
		// textureIndex = 1 points to the engine's built-in solid white fallback texture!
		Renderer::DrawUI(rc, panel->mesh, panel->textureIndex);
	}
}

} // namespace ZHLN
