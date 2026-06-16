#include "LightingSystem.hpp"

#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Entity.hpp"
#include "Zahlen/Render.hpp"
#include "Zahlen/Scripting.h"
#include "Zahlen/Types.hpp"
#include "ecs/ECS.hpp"

namespace ZHLN {

void LightingSystem::Update(Engine& engine, [[maybe_unused]] float dt) {
	auto& reg = engine.GetRegistry();
	auto& rc = engine.GetRenderContext();

	ZHLN_GameState state =
		*static_cast<ZHLN_GameState*>(ZHLN_GetGameState(reinterpret_cast<ZHLN_Engine*>(&engine)));

	std::vector<GPULight> sceneLights;
	for (Entity e : reg.GetEntitiesWith<LightComponent>()) {
		auto* light = reg.Get<LightComponent>(e);
		auto* trans = reg.Get<TransformComponent>(e);

		GPULight gpuLight{};
		gpuLight.type = light->type;
		gpuLight.color[0] = light->color.GetX();
		gpuLight.color[1] = light->color.GetY();
		gpuLight.color[2] = light->color.GetZ();
		gpuLight.intensity = light->intensity;
		gpuLight.radius = light->radius;
		gpuLight.twoSided = light->twoSided;

		if (trans != nullptr) {
			gpuLight.position[0] = trans->position.GetX();
			gpuLight.position[1] = trans->position.GetY();
			gpuLight.position[2] = trans->position.GetZ();
		}

		if (gpuLight.type == 1 && gpuLight.color[2] > 0.9f) {
			gpuLight.intensity = state.light1Intensity;
			gpuLight.radius = state.sphereLightRadius;
		} else if (gpuLight.type == 1 && gpuLight.color[0] > 0.9f) {
			gpuLight.intensity = state.light2Intensity;
			gpuLight.radius = state.sphereLightRadius;
		}

		if (gpuLight.type == 3) {
			std::memcpy(gpuLight.points, &light->points, sizeof(JPH::Mat44));
		}

		sceneLights.push_back(gpuLight);
	}

	auto floorEntities = reg.GetEntitiesWith<NameComponent>();
	auto floorNames = reg.GetRawArray<NameComponent>();
	for (size_t i = 0; i < floorEntities.size(); ++i) {
		std::string nameLower(floorNames[i].name.c_str());
		std::ranges::transform(nameLower, nameLower.begin(), ::tolower);
		if (nameLower.contains("floor") || nameLower.contains("ground") ||
			nameLower.contains("lobby")) {
			if (auto* floorMeshComp = reg.Get<MeshComponent>(floorEntities[i])) {
				floorMeshComp->material.roughnessFactor = state.floorRoughness;
				floorMeshComp->material.metallicFactor = state.floorMetallic;
			}
		}
	}

	Renderer::SetLights(rc, sceneLights.data(), sceneLights.size());
}

} // namespace ZHLN
