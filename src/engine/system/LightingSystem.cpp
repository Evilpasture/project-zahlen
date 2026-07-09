// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "LightingSystem.hpp"

#include "Zahlen/Camera.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Entity.hpp"
#include "Zahlen/Render.hpp"
#include "Zahlen/Types.hpp"
#include "detail/Array.hpp"
#include "ecs/ECS.hpp"

#include <algorithm>
#include <cstring>

namespace ZHLN {

std::pair<JPH::Vec3, float>
LightingSystem::GetSunDirectionAndIntensity(const ECS::Registry& reg) noexcept {
	JPH::Vec3 sunDirection = {0.5f, 1.0f,
							  0.2f}; // Default reference direction matching baked skybox
	float sunIntensity = 180.0f;	 // Default fallback solar constant
	bool sunFound = false;

	// Search for any Components::LightComponent explicitly marked as LightType::Sun
	for (Entity e : reg.GetEntitiesWith<Components::LightComponent>()) {
		auto* light = reg.Get<Components::LightComponent>(e);
		if (light->type == LightType::Sun) {
			if (auto* trans = reg.Get<Components::Components::TransformComponent>(e)) {
				JPH::Mat44 worldMat = trans->GetMatrix();
				// Local +Z is backward (pointing TO the sun) because local -Z is forward (pointing
				// away)
				sunDirection = worldMat.GetColumn3(2);
				sunIntensity = light->intensity;
				sunFound = true;
				break;
			}
		}
	}

	// Fallback to legacy tag search if no explicit Sun type was registered
	if (!sunFound) {
		auto sunEntities = reg.GetEntitiesWith<Components::SunTagComponent>();
		if (!sunEntities.empty()) {
			Entity sunEnt = sunEntities[0];
			if (auto* trans = reg.Get<Components::Components::TransformComponent>(sunEnt)) {
				JPH::Mat44 worldMat = trans->GetMatrix();
				sunDirection = worldMat.GetColumn3(2);
			}
			if (auto* light = reg.Get<Components::LightComponent>(sunEnt)) {
				sunIntensity = light->intensity;
			}
		}
	}

	return {sunDirection.Normalized(), sunIntensity};
}

void LightingSystem::Update(Engine& engine, [[maybe_unused]] float dt) {
	auto& reg = engine.GetRegistry();
	auto& rc = engine.GetRenderContext();

	// 1. DYNAMIC SHADOW ALLOCATION FOR PUNCTUAL LIGHTS
	Entity playerEnt = NullEntity;
	for (Entity e : reg.GetEntitiesWith<Components::PlayerTagComponent>()) {
		playerEnt = e;
		break;
	}

	if (playerEnt != NullEntity) {
		struct LightDistance {
			Entity entity;
			float distSq;
		};
		ZHLN::Array<LightDistance> lightDistances;

		if (auto* playerTrans = reg.Get<Components::Components::TransformComponent>(playerEnt)) {
			JPH::Vec3 playerPos = playerTrans->position;

			for (Entity e : reg.GetEntitiesWith<Components::LightComponent>()) {
				auto* light = reg.Get<Components::LightComponent>(e);
				light->shadowLayer = -1; // Reset to disabled initially

				// Punctual shadows are only allocated to local point/spot lights
				if (light->type == LightType::Point || light->type == LightType::Spot) {
					if (auto* trans = reg.Get<Components::Components::TransformComponent>(e)) {
						float dSq = (trans->position - playerPos).LengthSq();
						lightDistances.push_back({.entity = e, .distSq = dSq});
					}
				}
			}

			// Sort light sources nearest to player
			std::ranges::sort(lightDistances, [](const LightDistance& a, const LightDistance& b) {
				return a.distSq < b.distSq;
			});

			auto shadowEntities = reg.GetEntitiesWith<Components::ShadowSettingsComponent>();
			if (!shadowEntities.empty()) {
				auto* shadowSettings =
					reg.Get<Components::ShadowSettingsComponent>(shadowEntities[0]);
				uint32_t shadowCasters =
					std::min(static_cast<uint32_t>(shadowSettings->maxPunctualShadows),
							 static_cast<uint32_t>(lightDistances.size()));
				for (uint32_t i = 0; i < shadowCasters; ++i) {
					reg.Get<Components::LightComponent>(lightDistances[i].entity)->shadowLayer = i;
				}
			}
		}
	}

	// 2. COMPILE GPU LIGHTS
	ZHLN::Array<GPULight> sceneLights;
	JPH::Mat44 viewMatrix = engine.GetCamera().GetViewMatrix();
	auto lightEntities = reg.GetEntitiesWith<Components::LightComponent>();
	sceneLights.reserve(lightEntities.size());

	for (Entity e : lightEntities) {
		auto* light = reg.Get<Components::LightComponent>(e);
		auto* trans = reg.Get<Components::Components::TransformComponent>(e);

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

			// Transform position to view-space for cluster culling
			JPH::Vec3 posView = viewMatrix * trans->position;
			gpuLight.positionView[0] = posView.GetX();
			gpuLight.positionView[1] = posView.GetY();
			gpuLight.positionView[2] = posView.GetZ();

			if (light->type == LightType::Directional || light->type == LightType::Spot ||
				light->type == LightType::Sun) {
				JPH::Mat44 worldMat = trans->GetMatrix();
				JPH::Vec3 dir = -worldMat.GetColumn3(2);
				dir = dir.Normalized();
				gpuLight.direction[0] = dir.GetX();
				gpuLight.direction[1] = dir.GetY();
				gpuLight.direction[2] = dir.GetZ();
			}
		}

		if (gpuLight.type == LightType::Area) {
			std::memcpy(gpuLight.points, &light->points, sizeof(JPH::Mat44));
		}

		sceneLights.push_back(gpuLight);
	}

	Renderer::SetLights(rc, sceneLights.data(), sceneLights.size());
}

} // namespace ZHLN
