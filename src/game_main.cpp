// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/game_main.cpp

#include "Zahlen/CommandLine.hpp"
#include "Zahlen/Input.hpp"
#include "Zahlen/alife/Types.hpp"
#include "ecs/ECS.hpp"
#include "engine/FileWatcher.hpp"
#include "engine/Platform.hpp"
#include "engine/system/PhysicsStateSystem.hpp"
#include "engine/system/TargetCameraSystem.hpp"
#include "engine/system/TransformSystem.hpp"
#include "imgui.h"
#include "physics/Physics.hpp"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Scripting.h>
#include <Zahlen/Scripting.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <detail/ControlFlow.hpp>
#include <engine/system/AnimationSystem.hpp>
#include <engine/system/ArticulationSystem.hpp>
#include <engine/system/CullingSystem.hpp>
#include <expected>
#include <physics/PhysicsWorld.hpp>
#include <string>
#include <thread>
#include <threading/Mutex.hpp>
#include <threading/TaskSystem.hpp>
#include <utility> // Required for std::move

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace ZHLN {

// ============================================================================
// ECS Components
// ============================================================================

struct InputComponent {
	float localMoveX = 0.0f;
	float localMoveZ = 0.0f;
	float lookYawDelta = 0.0f;
	float lookPitchDelta = 0.0f;
	float zoomDelta = 0.0f;
	bool wantsToJump = false;
	bool wantsToSprint = false;
};

struct LightComponent {
	uint32_t type; // 0=Dir, 1=Point, 2=Spot, 3=Area (LTC Quad)
	JPH::Vec3 color;
	float intensity;
	float radius;
	JPH::Vec3 direction;
	float range;
	float points[4][4];
	uint32_t twoSided;
};

struct PostProcessComponent {
	int giMode;
	float aoRadius;
	float aoBias;
	float aoPower;
	float giIntensity;
	int giSamples;
	int useLocalProbe;
	JPH::Vec3 probeMin;
	JPH::Vec3 probeMax;
	JPH::Vec3 probePos;
	float vignetteIntensity;
	float vignettePower;
	int enableSSR;
	int enableRTR;
	int enableTAA;
	float taaFeedback;
};

struct CameraComponent {
	JPH::Mat44 viewProj = JPH::Mat44::sIdentity();
	JPH::Mat44 unjitteredViewProj = JPH::Mat44::sIdentity();
	JPH::Mat44 prevUnjitteredViewProj = JPH::Mat44::sIdentity();
	JPH::Mat44 frozenViewProj = JPH::Mat44::sIdentity();
	uint32_t frameCounter = 0;
};

struct GameContext {
	Entity playerEntity = NullEntity;
	Entity cameraEntity = NullEntity;
	Entity settingsEntity = NullEntity;
	uint32_t fontAtlasIdx = 0;
	Mesh helloText{};
	JPH::Array<Entity> visibleEntities;

	ScriptRunner* scriptRunner = nullptr;
	FileWatcher* gameplayWatcher = nullptr;
	ArticulationSystem* articulationSystem = nullptr;
	AnimationSystem* animationSystem = nullptr;
	TransformSystem* transformSystem = nullptr;

	TAAState taaState{};
	std::array<JPH::Vec3, 8> frustumCorners{};
};

void DrawConsole(ScriptRunner& runner);
void DrawProfiler(Engine& engine, TAAState& taaState);
void MovementSystem(Engine& engine, float dt);
void AudioSystem(Engine& engine, float dt);
void DrawOrientationGizmo(const ZHLN::Camera& cam);
void DrawInventoryShell(ScriptRunner& runner);
void DrawECSProfiler();

namespace {

struct FrustumEdge {
	int start;
	int end;
};

constexpr std::array<FrustumEdge, 12> s_FrustumEdges = {{
	{.start = 0, .end = 1},
	{.start = 1, .end = 2},
	{.start = 2, .end = 3},
	{.start = 3, .end = 0}, // Near Plane loop
	{.start = 4, .end = 5},
	{.start = 5, .end = 6},
	{.start = 6, .end = 7},
	{.start = 7, .end = 4}, // Far Plane loop
	{.start = 0, .end = 4},
	{.start = 1, .end = 5},
	{.start = 2, .end = 6},
	{.start = 3, .end = 7} // Near-to-Far connection lines
}};

} // namespace

// ============================================================================
// Modular Systems
// ============================================================================

void InputSystem(Engine& engine) {
	auto& input = engine.GetInput();
	auto& reg = engine.GetRegistry();
	auto mouse = input.GetMouse();

	for (Entity e : reg.GetEntitiesWith<InputComponent>()) {
		if (auto* ic = reg.Get<InputComponent>(e)) {
			float moveX = 0.0f;
			float moveZ = 0.0f;
			if (input.IsKeyDown(KeyCode::W)) {
				moveZ += 1.0f;
			}
			if (input.IsKeyDown(KeyCode::S)) {
				moveZ -= 1.0f;
			}
			if (input.IsKeyDown(KeyCode::A)) {
				moveX -= 1.0f;
			}
			if (input.IsKeyDown(KeyCode::D)) {
				moveX += 1.0f;
			}

			float len = std::sqrt(moveX * moveX + moveZ * moveZ);
			if (len > 0.001f) {
				moveX /= len;
				moveZ /= len;
			}
			ic->localMoveX = moveX;
			ic->localMoveZ = moveZ;

			if (input.IsMouseButtonDown(KeyCode::RButton)) {
				const float sensitivity = 0.15f;
				ic->lookYawDelta = mouse.deltaX * sensitivity;
				ic->lookPitchDelta = mouse.deltaY * sensitivity;
			} else {
				ic->lookYawDelta = 0.0f;
				ic->lookPitchDelta = 0.0f;
			}

			if (std::abs(mouse.wheel) > 0.01f) {
				ic->zoomDelta = mouse.wheel * 0.5f;
			} else {
				ic->zoomDelta = 0.0f;
			}

			ic->wantsToJump = input.IsKeyDown(KeyCode::Space);
			ic->wantsToSprint = input.IsKeyDown(KeyCode::LShift);
		}
	}
}

void PlayerInputTranslationSystem(Engine& engine, const Camera& cam) {
	auto& reg = engine.GetRegistry();
	for (Entity e : reg.GetEntitiesWith<MovementComponent>()) {
		auto* move = reg.Get<MovementComponent>(e);
		auto* input = reg.Get<InputComponent>(e);
		if ((move == nullptr) || (input == nullptr)) {
			continue;
		}

		float yawRad = JPH::DegreesToRadians(cam.yaw);
		float forward_x = std::cos(yawRad);
		float forward_z = std::sin(yawRad);
		float right_x = -std::sin(yawRad);
		float right_z = std::cos(yawRad);

		float worldX = (input->localMoveZ * forward_x) + (input->localMoveX * right_x);
		float worldZ = (input->localMoveZ * forward_z) + (input->localMoveX * right_z);

		float len = std::sqrt(worldX * worldX + worldZ * worldZ);
		if (len > 0.001f) {
			worldX /= len;
			worldZ /= len;
		}

		move->inputX = worldX;
		move->inputZ = worldZ;
		move->isSprinting = input->wantsToSprint && (len > 0.001f);
		if (input->wantsToJump) {
			move->jumpRequested = true;
		}
	}
}

void CameraSystem(Engine& engine, GameContext& game, float dt, float alpha) {
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();

	for (Entity camEnt : reg.GetEntitiesWith<TargetCameraComponent>()) {
		auto* camComp = reg.Get<TargetCameraComponent>(camEnt);
		auto* input = reg.Get<InputComponent>(camEnt);
		if ((camComp == nullptr) || (input == nullptr) || !reg.IsAlive(camComp->target)) {
			continue;
		}

		Entity targetEnt = camComp->target;
		JPH::Vec3 targetPos = JPH::Vec3::sZero();

		if (auto* state = reg.Get<PhysicsStateComponent>(targetEnt)) {
			float clampedAlpha = std::clamp(alpha, 0.0f, 1.0f);
			targetPos =
				state->prevPosition + clampedAlpha * (state->currPosition - state->prevPosition);
		} else if (auto* trans = reg.Get<TransformComponent>(targetEnt)) {
			targetPos = JPH::Vec3(trans->position[0], trans->position[1], trans->position[2]);
		} else if (auto* meshComp = reg.Get<MeshComponent>(targetEnt)) {
			targetPos = meshComp->localTransform.GetTranslation();
		}

		if (std::abs(input->zoomDelta) > 1e-4f) {
			camComp->targetDistance =
				JPH::Clamp(camComp->targetDistance - input->zoomDelta, 1.5f, 15.0f);
		}

		if (camComp->stiffness > 0.0f) {
			float factor = JPH::Clamp(camComp->stiffness * dt, 0.0f, 1.0f);
			camComp->distance += (camComp->targetDistance - camComp->distance) * factor;
			camComp->fov += (camComp->targetFov - camComp->fov) * factor;
		} else {
			camComp->distance = camComp->targetDistance;
			camComp->fov = camComp->targetFov;
		}

		camComp->yaw += input->lookYawDelta;
		camComp->pitch = std::clamp(camComp->pitch - input->lookPitchDelta, -89.0f, 89.0f);

		cam.yaw = camComp->yaw;
		cam.pitch = camComp->pitch;
		cam.fov = camComp->fov;

		float yawRad = JPH::DegreesToRadians(camComp->yaw);
		float pitchRad = JPH::DegreesToRadians(camComp->pitch);
		JPH::Vec3 offsetDir(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad),
							JPH::Sin(yawRad) * JPH::Cos(pitchRad));

		JPH::Vec3 offsetVec(camComp->targetOffset[0], camComp->targetOffset[1],
							camComp->targetOffset[2]);
		JPH::Vec3 smoothTargetPos(camComp->smoothTargetPos[0], camComp->smoothTargetPos[1],
								  camComp->smoothTargetPos[2]);

		if (camComp->hasInitSmoothTarget == 0) {
			smoothTargetPos = targetPos;
			camComp->hasInitSmoothTarget = 1;
		}

		if ((targetPos - smoothTargetPos).LengthSq() > 100.0f) {
			smoothTargetPos = targetPos;
		} else if (camComp->stiffness > 0.0f) {
			float factor = 1.0f - std::exp(-camComp->stiffness * dt);
			smoothTargetPos += (targetPos - smoothTargetPos) * factor;
		} else {
			smoothTargetPos = targetPos;
		}

		camComp->smoothTargetPos[0] = smoothTargetPos.GetX();
		camComp->smoothTargetPos[1] = smoothTargetPos.GetY();
		camComp->smoothTargetPos[2] = smoothTargetPos.GetZ();

		cam.position = smoothTargetPos - (offsetDir.Normalized() * camComp->distance) + offsetVec;
	}

	auto res = engine.GetWindow().GetSize();
	if (res.width == 0 || res.height == 0) {
		return;
	}

	for (Entity e : reg.GetEntitiesWith<CameraComponent>()) {
		if (auto* cComp = reg.Get<CameraComponent>(e)) {
			if (cComp->frameCounter == 0) {
				cComp->prevUnjitteredViewProj =
					cam.GetProjectionMatrix((float)res.width / res.height) * cam.GetViewMatrix();
				cComp->unjitteredViewProj = cComp->prevUnjitteredViewProj;
				cComp->viewProj = cComp->unjitteredViewProj;
			} else {
				cComp->prevUnjitteredViewProj = cComp->unjitteredViewProj;
			}

			JPH::Mat44 unjitteredProj = cam.GetProjectionMatrix((float)res.width / res.height);
			cComp->unjitteredViewProj = unjitteredProj * cam.GetViewMatrix();

			if (game.taaState.enabled) {
				game.taaState.frameIndex++;
				cComp->viewProj =
					cam.GetJitteredProjectionMatrix((float)res.width / res.height, res.width,
													res.height, game.taaState) *
					cam.GetViewMatrix();
			} else {
				game.taaState.frameIndex = 0;
				cComp->viewProj = cComp->unjitteredViewProj;
			}

			static bool s_WasFrozen = false;
			if (CullingStats::FreezeFrustum) {
				if (!s_WasFrozen) {
					cComp->frozenViewProj = cComp->unjitteredViewProj;
					JPH::Mat44 invVP = cComp->unjitteredViewProj.Inversed();
					auto ndc = std::to_array<JPH::Vec4>({{-1.0f, -1.0f, 0.0f, 1.0f},
														 {1.0f, -1.0f, 0.0f, 1.0f},
														 {1.0f, 1.0f, 0.0f, 1.0f},
														 {-1.0f, 1.0f, 0.0f, 1.0f},
														 {-1.0f, -1.0f, 1.0f, 1.0f},
														 {1.0f, -1.0f, 1.0f, 1.0f},
														 {1.0f, 1.0f, 1.0f, 1.0f},
														 {-1.0f, 1.0f, 1.0f, 1.0f}});
					for (int i = 0; i < 8; ++i) {
						JPH::Vec4 worldPos = invVP * ndc[i];
						float w = worldPos.GetW();
						if (std::abs(w) > 1e-6f) {
							game.frustumCorners[i] = JPH::Vec3(
								worldPos.GetX() / w, worldPos.GetY() / w, worldPos.GetZ() / w);
						}
					}
					s_WasFrozen = true;
				}
				cam.frustum.Update(cComp->frozenViewProj);
			} else {
				cam.frustum.Update(cComp->unjitteredViewProj);
				s_WasFrozen = false;
			}

			cComp->frameCounter++;
		}
	}
}

void LightingSystem(Engine& engine) {
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
			std::memcpy(gpuLight.points, light->points, sizeof(float) * 16);
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

void UISystem(Engine& engine, GameContext& game) {
	if (engine.GetWindow().IsTTY()) {
		return;
	}

	ZHLN_GameState state =
		*static_cast<ZHLN_GameState*>(ZHLN_GetGameState(reinterpret_cast<ZHLN_Engine*>(&engine)));

	ZHLN::DrawConsole(*game.scriptRunner);
	ZHLN::DrawInventoryShell(*game.scriptRunner);
	ZHLN::DrawProfiler(engine, game.taaState);
	ZHLN::DrawOrientationGizmo(engine.GetCamera());
	ZHLN::DrawECSProfiler();

	ImGui::Begin("Lighting Workspace Controller");
	ImGui::Text("Specular Mips & Area Lights Debugger");
	ImGui::Separator();
	ImGui::SliderFloat("Sphere Light Radius", &state.sphereLightRadius, 0.0f, 5.0f);
	ImGui::SliderFloat("Cyan Intensity", &state.light1Intensity, 0.0f, 500.0f);
	ImGui::SliderFloat("Magenta Intensity", &state.light2Intensity, 0.0f, 500.0f);
	ImGui::Separator();
	ImGui::SliderFloat("Floor Roughness", &state.floorRoughness, 0.0f, 1.0f);
	ImGui::SliderFloat("Floor Metallic", &state.floorMetallic, 0.0f, 1.0f);

	ImGui::SeparatorText("Parallax-Corrected Local Reflection Probe");
	bool useProbe = state.useLocalProbe != 0;
	if (ImGui::Checkbox("Enable Box Projection", &useProbe)) {
		state.useLocalProbe = useProbe ? 1 : 0;
	}
	if (state.useLocalProbe != 0) {
		std::array<float, 3> minArr = {state.probeMin[0], state.probeMin[1], state.probeMin[2]};
		std::array<float, 3> maxArr = {state.probeMax[0], state.probeMax[1], state.probeMax[2]};
		std::array<float, 3> posArr = {state.probePos[0], state.probePos[1], state.probePos[2]};

		if (ImGui::DragFloat3("Box Min", minArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
			state.probeMin[0] = minArr[0];
			state.probeMin[1] = minArr[1];
			state.probeMin[2] = minArr[2];
		}
		if (ImGui::DragFloat3("Box Max", maxArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
			state.probeMax[0] = maxArr[0];
			state.probeMax[1] = maxArr[1];
			state.probeMax[2] = maxArr[2];
		}
		if (ImGui::DragFloat3("Probe Position", posArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
			state.probePos[0] = posArr[0];
			state.probePos[1] = posArr[1];
			state.probePos[2] = posArr[2];
		}
	}

	ImGui::SeparatorText("Ambient Occlusion & Global Illumination");
	const char* giModesList[] = {"Off", "SSAO (Ambient Occlusion)", "SSGI (Screen Space GI)",
								 "HBAO (Horizon-Based AO)", "GTAO (Ground Truth AO)"};
	ImGui::Combo("GI Mode", &state.giMode, giModesList, IM_ARRAYSIZE(giModesList));

	if (state.giMode == 1) {
		ImGui::SliderFloat("AO Radius", &state.aoRadius, 0.05f, 2.5f, "%.2fm");
		ImGui::SliderFloat("AO Bias", &state.aoBias, 0.001f, 0.2f, "%.3f");
		ImGui::SliderFloat("AO Contrast", &state.aoPower, 0.5f, 5.0f, "%.1fx");
		ImGui::SliderInt("AO Samples", &state.giSamples, 2, 32);
	} else if (state.giMode == 2) {
		ImGui::SliderFloat("Bounce Radius", &state.aoRadius, 0.05f, 2.5f, "%.2fm");
		ImGui::SliderFloat("Bounce Bias", &state.aoBias, 0.001f, 0.2f, "%.3f");
		ImGui::SliderFloat("GI Bounce Intensity", &state.giIntensity, 0.1f, 5.0f, "%.1fx");
		ImGui::SliderInt("GI Samples", &state.giSamples, 2, 32);
	} else if (state.giMode == 3 || state.giMode == 4) {
		ImGui::SliderFloat("Search Radius", &state.aoRadius, 0.05f, 3.0f, "%.2fm");
		ImGui::SliderFloat("Acne Bias", &state.aoBias, 0.001f, 0.2f, "%.3f");
		ImGui::SliderFloat("Shadow Contrast", &state.aoPower, 0.5f, 6.0f, "%.1fx");
		ImGui::SliderInt("Search Steps", &state.giSamples, 4, 32);
	}

	ImGui::SeparatorText("Camera Vignette");
	ImGui::SliderFloat("Vignette Intensity", &state.vignetteIntensity, 0.0f, 2.5f, "%.2f");
	if (state.vignetteIntensity > 0.0f) {
		ImGui::SliderFloat("Vignette Power", &state.vignettePower, 0.1f, 6.0f, "%.2f");
	}

	bool useSsr = state.enableSSR != 0;
	if (ImGui::Checkbox("Enable SSR", &useSsr))
		state.enableSSR = useSsr ? 1 : 0;

	bool useRtr = state.enableRTR != 0;
	if (ImGui::Checkbox("Enable Hardware RTR", &useRtr))
		state.enableRTR = useRtr ? 1 : 0;

	ImGui::End();

	ZHLN_SetGameState(reinterpret_cast<ZHLN_Engine*>(&engine), &state);

	auto& reg = engine.GetRegistry();
	for (Entity e : reg.GetEntitiesWith<PostProcessComponent>()) {
		if (auto* pp = reg.Get<PostProcessComponent>(e)) {
			pp->giMode = state.giMode;
			pp->aoRadius = state.aoRadius;
			pp->aoBias = state.aoBias;
			pp->aoPower = state.aoPower;
			pp->giIntensity = state.giIntensity;
			pp->giSamples = state.giSamples;
			pp->useLocalProbe = state.useLocalProbe;
			pp->probeMin = JPH::Vec3(state.probeMin[0], state.probeMin[1], state.probeMin[2]);
			pp->probeMax = JPH::Vec3(state.probeMax[0], state.probeMax[1], state.probeMax[2]);
			pp->probePos = JPH::Vec3(state.probePos[0], state.probePos[1], state.probePos[2]);
			pp->vignetteIntensity = state.vignetteIntensity;
			pp->vignettePower = state.vignettePower;
			pp->enableSSR = state.enableSSR;
			pp->enableRTR = state.enableRTR;
			pp->enableTAA = state.enableTAA;
			pp->taaFeedback = state.taaFeedback;
		}
	}
}

void PostProcessSystem(Engine& engine, GameContext& game) {
	auto& reg = engine.GetRegistry();
	auto& rc = engine.GetRenderContext();

	for (Entity e : reg.GetEntitiesWith<PostProcessComponent>()) {
		if (auto* pp = reg.Get<PostProcessComponent>(e)) {
			game.taaState.enabled = pp->enableTAA != 0;
			game.taaState.feedback = pp->taaFeedback;

			Renderer::SetGISettings(rc, {.mode = pp->giMode,
										 .aoRadius = pp->aoRadius,
										 .aoBias = pp->aoBias,
										 .aoPower = pp->aoPower,
										 .giIntensity = pp->giIntensity,
										 .giSamples = pp->giSamples,
										 .vignetteIntensity = pp->vignetteIntensity,
										 .vignettePower = pp->vignettePower,
										 .enableSSR = pp->enableSSR ? 1 : 0,
										 .enableRTR = pp->enableRTR ? 1 : 0});
		}
	}
}

void DebugDrawSystem(Engine& engine, GameContext& game) {
	if (!CullingStats::FreezeFrustum)
		return;

	auto& rc = engine.GetRenderContext();
	ZHLN_GameState state =
		*static_cast<ZHLN_GameState*>(ZHLN_GetGameState(reinterpret_cast<ZHLN_Engine*>(&engine)));

	if (state.debugLineVbo != 0) {
		Mesh debugMesh = {.vertexBuffer = static_cast<BufferHandle>(state.debugLineVbo),
						  .vertexCount = 36};
		Material debugMat = {.pipeline = static_cast<PipelineHandle>(state.debugLinePipeline),
							 .albedoIndex = state.debugLineAlbedo};

		debugMat.baseColorFactor[0] = 0.0f;
		debugMat.baseColorFactor[1] = 1.0f;
		debugMat.baseColorFactor[2] = 1.0f;
		debugMat.baseColorFactor[3] = 1.0f;

		for (auto s_FrustumEdge : s_FrustumEdges) {
			JPH::Vec3 pA = game.frustumCorners[s_FrustumEdge.start];
			JPH::Vec3 pB = game.frustumCorners[s_FrustumEdge.end];

			JPH::Vec3 v = pB - pA;
			float len = v.Length();
			if (len < 1e-4f)
				continue;

			JPH::Vec3 dir = v / len;
			JPH::Vec3 mid = (pA + pB) * 0.5f;

			JPH::Quat rot = JPH::Quat::sFromTo(JPH::Vec3::sAxisZ(), dir);
			JPH::Mat44 lineTransform = Math::CreateTransform(mid, rot, JPH::Vec3(1.0f, 1.0f, len));

			Renderer::Draw(
				rc, debugMat, debugMesh,
				{.transform = lineTransform, .prevTransform = lineTransform, .cullRadius = len});
		}
	}
}

void RenderSystem(Engine& engine, GameContext& game) {
	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();

	JPH::Mat44 vp, unjitteredVp, prevUnjitteredVp;
	if (auto* cComp = reg.Get<CameraComponent>(game.cameraEntity)) {
		vp = cComp->viewProj;
		unjitteredVp = cComp->unjitteredViewProj;
		prevUnjitteredVp = cComp->prevUnjitteredViewProj;
	} else {
		return;
	}

	int enableRTR = 0;
	JPH::Vec4 probeMin(0, 0, 0, 0), probeMax(0, 0, 0, 0), probePos(0, 0, 0, 0);
	if (auto* pp = reg.Get<PostProcessComponent>(game.settingsEntity)) {
		enableRTR = pp->enableRTR;
		probeMin = JPH::Vec4(pp->probeMin.GetX(), pp->probeMin.GetY(), pp->probeMin.GetZ(),
							 pp->useLocalProbe ? 1.0f : 0.0f);
		probeMax = JPH::Vec4(pp->probeMax.GetX(), pp->probeMax.GetY(), pp->probeMax.GetZ(), 0.0f);
		probePos = JPH::Vec4(pp->probePos.GetX(), pp->probePos.GetY(), pp->probePos.GetZ(), 0.0f);
	}

	JPH::Vec3 sunDirection = {0.5f, 1.0f, 0.2f};
	JPH::Mat44 lightView =
		Math::CreateLookAt(sunDirection * 100.0f, {0.0f, 0.0f, 0.0f}, JPH::Vec3::sAxisY());
	JPH::Mat44 lightProj = Math::CreateOrtho(-50.0f, 50.0f, -50.0f, 50.0f, 0.1f, 200.0f);
	JPH::Mat44 shadowProjView = lightProj * lightView;

	JPH::Mat44 biasMatrix = {JPH::Vec4(0.5f, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, -0.5f, 0.0f, 0.0f),
							 JPH::Vec4(0.0f, 0.0f, 1.0f, 0.0f), JPH::Vec4(0.5f, 0.5f, 0.0f, 1.0f)};
	JPH::Mat44 lightSpaceBiased = biasMatrix * shadowProjView;

	FrameUniforms uniforms{};
	uniforms.viewProj = vp;
	uniforms.unjitteredViewProj = unjitteredVp;
	uniforms.prevUnjitteredViewProj = prevUnjitteredVp;
	uniforms.lightSpaceMatrix = lightSpaceBiased;
	uniforms.invViewProj = unjitteredVp.Inversed();
	std::memcpy(&uniforms.camPos[0], &cam.position, sizeof(float) * 3);
	std::memcpy(&uniforms.lightDir[0], &sunDirection, sizeof(float) * 3);
	uniforms.lightCount = reg.GetEntitiesWith<LightComponent>().size();
	uniforms.probeMin = probeMin;
	uniforms.probeMax = probeMax;
	uniforms.probePos = probePos;
	uniforms.jitterParams = JPH::Vec4(game.taaState.jitterX, game.taaState.jitterY,
									  game.taaState.prevJitterX, game.taaState.prevJitterY);
	uniforms.enableRTR = enableRTR;

	rc.SetTAAState(game.taaState);
	Renderer::SetFrameData(rc, uniforms, shadowProjView);
	Renderer::SetMatrices(rc, vp, unjitteredVp);

	engine.BeginFrame();

	for (Entity e : game.visibleEntities) {
		auto* mesh = reg.Get<MeshComponent>(e);
		if (!mesh)
			continue;

		DrawFlags flags = DrawFlags::None;
		if (mesh->isSkinned)
			flags |= DrawFlags::Skinned;

		Renderer::Draw(rc, mesh->material, mesh->mesh,
					   {.transform = mesh->worldTransform,
						.prevTransform = mesh->prevTransform,
						.cullRadius = mesh->cullRadius,
						.jointOffset = mesh->jointOffset,
						.morphOffset = mesh->morphOffset,
						.activeMorphCount = mesh->activeMorphCount,
						.morphWeights = mesh->morphWeights,
						.flags = flags});
	}

	CullingStats::TotalObjects = reg.GetEntitiesWith<MeshComponent>().size();
	CullingStats::CulledObjects = CullingStats::TotalObjects - game.visibleEntities.size();

	Renderer::DrawUI(rc, game.helloText, game.fontAtlasIdx);

	DebugDrawSystem(engine, game);

	engine.EndFrame();
}

// ============================================================================
// GAME APPLICATION INTERFACE IMPLEMENTATION
// ============================================================================

bool InitializeGame(Engine& engine, GameContext& game) {
	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();
	auto& pc = engine.GetPhysicsContext();

	ZHLN_GameState defaultState{};
	defaultState.giMode = 1;
	defaultState.aoRadius = 0.5f;
	defaultState.aoBias = 0.05f;
	defaultState.aoPower = 1.8f;
	defaultState.giIntensity = 1.2f;
	defaultState.giSamples = 8;
	defaultState.useLocalProbe = 1;
	defaultState.probeMin[0] = -22.0f;
	defaultState.probeMin[1] = 0.0f;
	defaultState.probeMin[2] = -22.0f;
	defaultState.probeMax[0] = 22.0f;
	defaultState.probeMax[1] = 12.0f;
	defaultState.probeMax[2] = 22.0f;
	defaultState.probePos[0] = 0.0f;
	defaultState.probePos[1] = 4.0f;
	defaultState.probePos[2] = 0.0f;
	defaultState.vignetteIntensity = 1.10f;
	defaultState.vignettePower = 1.50f;
	defaultState.enableSSR = 1;
	defaultState.floorRoughness = 0.15f;
	defaultState.floorMetallic = 0.95f;
	defaultState.sphereLightRadius = 1.5f;
	defaultState.light1Intensity = 180.0f;
	defaultState.light2Intensity = 180.0f;
	defaultState.enableTAA = 1;
	defaultState.taaFeedback = 0.95f;

	ZHLN_SetGameState(reinterpret_cast<ZHLN_Engine*>(&engine), &defaultState);

	reg.RegisterComponents<TransformComponent, MeshComponent, PhysicsComponent,
						   PhysicsStateComponent, MovementComponent, ALife::ALifeComponent,
						   RagdollComponent, NameComponent, TargetCameraComponent, InputComponent,
						   LightComponent, PostProcessComponent, CameraComponent>();

	auto groundShape =
		Physics::GetOrCreateShape(pc, Physics::ShapeType::Plane, 0.0f, 1.0f, 0.0f, 0.0f);
	Entity ground = reg.Create();
	reg.Add(ground,
			PhysicsComponent{Physics::CreateRigidBody(
				pc, groundShape, {0, 0, 0}, JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0)});
	reg.Add(ground, PhysicsStateComponent{});

	game.playerEntity = reg.Create();
	reg.Add(game.playerEntity, TransformComponent{.position = {0.0f, 3.0f, 0.0f}});
	reg.Add(game.playerEntity, MovementComponent{});
	reg.Add(game.playerEntity, InputComponent{});
	Entity charPhys = Physics::CreateCharacter(pc, JPH::RVec3(0.0f, 3.0f, 0.0f));
	reg.Add(game.playerEntity, PhysicsComponent{charPhys});
	reg.Add(game.playerEntity, PhysicsStateComponent{.currPosition = {0.0f, 3.0f, 0.0f},
													 .prevPosition = {0.0f, 3.0f, 0.0f}});

	game.cameraEntity = reg.Create();
	reg.Add(game.cameraEntity, TargetCameraComponent{.target = game.playerEntity,
													 .distance = 4.5f,
													 .targetDistance = 4.5f,
													 .yaw = -90.0f,
													 .pitch = -10.0f,
													 .stiffness = 15.0f,
													 .vignetteIntensity = 1.10f,
													 .vignettePower = 1.50f,
													 .fov = 45.0f,
													 .targetFov = 45.0f});
	reg.Add(game.cameraEntity, InputComponent{});
	reg.Add(game.cameraEntity, CameraComponent{});

	// Construct PostProcessComponent directly as a temporary
	game.settingsEntity = reg.Create();
	reg.Add(game.settingsEntity,
			PostProcessComponent{
				.giMode = defaultState.giMode,
				.aoRadius = defaultState.aoRadius,
				.aoBias = defaultState.aoBias,
				.aoPower = defaultState.aoPower,
				.giIntensity = defaultState.giIntensity,
				.giSamples = defaultState.giSamples,
				.useLocalProbe = defaultState.useLocalProbe,
				.probeMin = JPH::Vec3(defaultState.probeMin[0], defaultState.probeMin[1],
									  defaultState.probeMin[2]),
				.probeMax = JPH::Vec3(defaultState.probeMax[0], defaultState.probeMax[1],
									  defaultState.probeMax[2]),
				.probePos = JPH::Vec3(defaultState.probePos[0], defaultState.probePos[1],
									  defaultState.probePos[2]),
				.vignetteIntensity = defaultState.vignetteIntensity,
				.vignettePower = defaultState.vignettePower,
				.enableSSR = defaultState.enableSSR,
				.enableRTR = defaultState.enableRTR,
				.enableTAA = defaultState.enableTAA,
				.taaFeedback = defaultState.taaFeedback});

	// Construct LightComponents directly as temporaries
	Entity areaLight = reg.Create();
	reg.Add(areaLight, LightComponent{.type = 3,
									  .color = {1.0f, 0.8f, 0.6f},
									  .intensity = 5.0f,
									  .radius = 0.0f,
									  .direction = {0.0f, -1.0f, 0.0f},
									  .range = 0.0f,
									  .points = {{-2.0f, 5.0f, -2.0f, 0.0f},
												 {2.0f, 5.0f, -2.0f, 0.0f},
												 {2.0f, 5.0f, 2.0f, 0.0f},
												 {-2.0f, 5.0f, 2.0f, 0.0f}},
									  .twoSided = 0});

	Entity pt1 = reg.Create();
	reg.Add(pt1,
			LightComponent{.type = 1,
						   .color = {0.0f, 0.5f, 1.0f},
						   .intensity = defaultState.light1Intensity,
						   .radius = defaultState.sphereLightRadius,
						   .direction = {0.0f, 0.0f, 0.0f},
						   .range = 0.0f,
						   .points = {},
						   .twoSided = 0},
			TransformComponent{.position = {-5.0f, 4.0f, 0.0f}});

	Entity pt2 = reg.Create();
	reg.Add(pt2,
			LightComponent{.type = 1,
						   .color = {1.0f, 0.0f, 0.5f},
						   .intensity = defaultState.light2Intensity,
						   .radius = defaultState.sphereLightRadius,
						   .direction = {0.0f, 0.0f, 0.0f},
						   .range = 0.0f,
						   .points = {},
						   .twoSided = 0},
			TransformComponent{.position = {5.0f, 4.0f, 0.0f}});

	game.scriptRunner = new ScriptRunner();
	game.gameplayWatcher = new FileWatcher("scripts/gameplay.lua");
	game.fontAtlasIdx = AssetFactory::CreateFontAtlasTexture(rc);
	game.helloText = GUI::CreateTextMesh(rc, "Zahlen Engine - TADC Dorm Showcase", 25.0f, 25.0f,
										 2.5f, {0.9f, 0.1f, 0.1f, 1.0f});

	game.articulationSystem = new ArticulationSystem();
	game.animationSystem = new AnimationSystem();
	game.transformSystem = new TransformSystem();

	return true;
}

void UpdateGame(Engine& engine, float dt, float& physicsAccumulator, GameContext& game) {
	InputSystem(engine);
	UISystem(engine, game);
	PostProcessSystem(engine, game);
	PlayerInputTranslationSystem(engine, engine.GetCamera());

	if constexpr (isDev) {
		static float watcherAccumulator = 0.0f;
		watcherAccumulator += dt;
		if (watcherAccumulator >= 2.0f) {
			watcherAccumulator = 0.0f;
			if (game.gameplayWatcher->CheckModified()) {
				game.scriptRunner->ReloadFile("scripts/gameplay.lua");
			}
		}
	}

	float cappedDt = std::min(dt, 0.1f);
	physicsAccumulator += cappedDt;
	constexpr float targetDt = 1.0f / 60.0f;
	physicsAccumulator = std::min(physicsAccumulator, targetDt * 4.0f);

	{
		ZHLN_PROFILE_SCOPE("ECS System: Physics & Movement");
		while (physicsAccumulator >= targetDt) {
			ZHLN::MovementSystem(engine, targetDt);
			engine.GetPhysicsContext().Step(targetDt);
			ZHLN::PhysicsStateSystem::WriteBack(engine);
			physicsAccumulator -= targetDt;
		}
	}

	float alpha = physicsAccumulator / targetDt;

	{
		ZHLN_PROFILE_SCOPE("ECS System: Visual Interpolation");
		ZHLN::VisualInterpolationSystem::Update(engine, alpha);
	}

	{
		ZHLN_PROFILE_SCOPE("ECS System: Script/Lua Update");
		game.scriptRunner->CallUpdate(&engine, dt);
	}

	{
		ZHLN_PROFILE_SCOPE("ECS System: Animation Update");
		game.animationSystem->UpdateAnimations(engine.GetRenderContext(), engine.GetRegistry(), dt);
	}

	{
		ZHLN_PROFILE_SCOPE("ECS System: Articulation/Ragdoll");
		game.articulationSystem->Update(engine, dt);
	}

	{
		ZHLN_PROFILE_SCOPE("ECS System: Resolve Transforms");
		game.transformSystem->ResolveTransforms(engine.GetRegistry());
	}
}

void RenderGame(Engine& engine, float frameTime, float physicsAccumulator, GameContext& game) {
	constexpr float targetDt = 1.0f / 60.0f;
	float alpha = physicsAccumulator / targetDt;

	CameraSystem(engine, game, frameTime, alpha);
	CullingSystem<false>(engine, game.visibleEntities);
	LightingSystem(engine);
	RenderSystem(engine, game);

	{
		ZHLN_PROFILE_SCOPE("ECS System: Audio Update");
		ZHLN::AudioSystem(engine, frameTime);
	}

	{
		ZHLN_PROFILE_SCOPE("ECS System: Update Transform History");
		game.transformSystem->UpdateTransformHistory(engine.GetRegistry());
	}
}

void ShutdownGame([[maybe_unused]] Engine& engine, GameContext& game) {
	delete game.scriptRunner;
	delete game.gameplayWatcher;
	delete game.articulationSystem;
	delete game.animationSystem;
	delete game.transformSystem;
}

} // namespace ZHLN

using namespace ZHLN;

namespace {

std::expected<std::unique_ptr<Engine>, EngineError> InitializeEngine(CommandLineOptions options) {
	Platform::Init();
	ZHLN::SetupSignalHandler();
	TaskSystem::Init();

	uint32_t w = options.fullscreen ? 0 : 1280;
	uint32_t h = options.fullscreen ? 0 : 720;

	EngineConfig config{
		.physics = {.maxBodies = 5000,
					.maxBodyPairs = 10000,
					.maxContactConstraints = 10000,
					.tempAllocatorSize = 64 * 1024 * 1024},
		.render = {.appName = "Zahlen Engine",
				   .width = w,
				   .height = h,
				   .vsync = options.vsync,
				   .fullscreen = options.fullscreen,
				   .enableValidation = options.enableValidation},
	};

	const char* initError = nullptr;
	auto engine = Engine::Create(config, &initError);

	if (!engine) {
		return std::unexpected(EngineError{
			.msg = (initError != nullptr) ? initError : "Unknown engine initialization error.",
			.code = EXIT_FAILURE});
	}

	engine->GetWindow().Focus();
	return engine;
}

std::expected<int, EngineError> RunEngineLoop(std::unique_ptr<Engine> engine, uint32_t fpsLimit) {
	Clock clock;
	GameContext game{};

	if (!ZHLN::InitializeGame(*engine, game)) {
		return std::unexpected(
			EngineError{.msg = "Game failed to initialize.", .code = EXIT_FAILURE});
	}

	for (int i = 0; i < 3; ++i) {
		engine->ProcessEvents();
		ZHLN::RenderGame(*engine, 0.016f, 0.0f, game);
	}

	ZHLN::Log("Window active and presenting. Loading scene assets...");
	game.scriptRunner->RunFile("scripts/gameplay.lua");

	float physicsAccumulator = 0.0f;
	const double targetFrameTime = fpsLimit > 0 ? 1.0 / static_cast<double>(fpsLimit) : 0.0;

	engine->ProcessEvents();
	clock.GetDeltaTime();
	auto frameStart = std::chrono::high_resolution_clock::now();

	while (engine->IsRunning()) {
		float frameTime = clock.GetDeltaTime();
		engine->ProcessEvents();

		if (engine->GetInput().IsKeyDown(KeyCode::Escape)) {
			engine->GetWindow().Close();
			break;
		}

		auto res = engine->GetWindow().GetSize();
		if (res.width <= 0 || res.height <= 0) {
			Platform::Sleep(10);
			continue;
		}

		if (engine->GetInput().NeedsResize()) {
			engine->GetRenderContext().SetResolution(engine->GetInput().GetNewSize());
			engine->GetInput().ClearResizeFlag();
			continue;
		}

		ZHLN::UpdateGame(*engine, frameTime, physicsAccumulator, game);
		ZHLN::RenderGame(*engine, frameTime, physicsAccumulator, game);

		if (fpsLimit > 0) {
			auto frameEnd = std::chrono::high_resolution_clock::now();
			double elapsed = std::chrono::duration<double>(frameEnd - frameStart).count();
			if (elapsed < targetFrameTime) {
				double sleepTime = targetFrameTime - elapsed;
				if (sleepTime > 0.002) {
					std::this_thread::sleep_for(
						std::chrono::microseconds(static_cast<int64_t>((sleepTime - 0.001) * 1e6)));
				}
				while (std::chrono::duration<double>(std::chrono::high_resolution_clock::now() -
													 frameStart)
						   .count() < targetFrameTime) {
#if defined(__x86_64__) || defined(_M_X64)
					_mm_pause();
#elif defined(__aarch64__)
					__asm__ __volatile__("yield" ::: "memory");
#else
					std::this_thread::yield();
#endif
				}
			}
		}
		frameStart = std::chrono::high_resolution_clock::now();
	}

	ZHLN::ShutdownGame(*engine, game);
	TaskSystem::Shutdown();

	return EXIT_SUCCESS;
}

} // namespace

extern auto RunGame(const ZHLN::CommandLineOptions& options) {
	auto result = InitializeEngine(options)
					  .and_then([&options](std::unique_ptr<Engine> engine) {
						  return RunEngineLoop(std::move(engine), options.fpsLimit);
					  })
					  .transform_error([](const EngineError& err) -> int {
						  if (!err.msg.empty() && !err.silent) {
							  ZHLN::Log("Error: {}", err.msg);
						  }
						  return err.code;
					  });

	return result.value_or(result.error());
}
