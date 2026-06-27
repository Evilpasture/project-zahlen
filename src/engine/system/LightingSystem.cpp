// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "LightingSystem.hpp"

#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Entity.hpp"
#include "Zahlen/Render.hpp"
#include "Zahlen/Types.hpp"
#include "ecs/ECS.hpp"

#include <cstring>
#include <vector>

namespace ZHLN {

void LightingSystem::Update(Engine& engine, [[maybe_unused]] float dt) {
	auto& reg = engine.GetRegistry();
	auto& rc = engine.GetRenderContext();

	std::vector<GPULight> sceneLights;
	auto lightEntities = reg.GetEntitiesWith<LightComponent>();
	sceneLights.reserve(lightEntities.size());

	for (Entity e : lightEntities) {
		auto* light = reg.Get<LightComponent>(e);
		auto* trans = reg.Get<TransformComponent>(e);

		GPULight gpuLight{};
		gpuLight.type = light->type;
		gpuLight.intensity = light->intensity;
		gpuLight.radius = light->radius;
		gpuLight.twoSided = light->twoSided;
		gpuLight.range = (light->range > 0.0f) ? light->range : 1000.0f;
		gpuLight.shadowLayer = light->shadowLayer;
		std::memcpy(gpuLight.direction, &light->direction, sizeof(float) * 3);
		std::memcpy(gpuLight.color, &light->color, sizeof(float) * 3);

		if (trans != nullptr) {
			std::memcpy(gpuLight.position, &trans->position, sizeof(float) * 3);
		}

		if (gpuLight.type == LightType::Area) {
			std::memcpy(gpuLight.points, &light->points, sizeof(JPH::Mat44));
		}

		sceneLights.push_back(gpuLight);
	}

	Renderer::SetLights(rc, sceneLights.data(), sceneLights.size());
}

} // namespace ZHLN
