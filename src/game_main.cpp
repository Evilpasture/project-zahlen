// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/game_main.cpp

#include "Zahlen/CommandLine.hpp"
#include "Zahlen/Input.hpp"
#include "Zahlen/alife/Types.hpp"
#include "ecs/ECS.hpp"
#include "engine/FileWatcher.hpp"
#include "engine/Platform.hpp"
#include "engine/system/TargetCameraSystem.hpp"
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
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace ZHLN {

struct GameContext {
	Entity playerEntity = NullEntity;
	float camDistance = 4.5f;
	uint32_t frameCounter = 0;
	uint32_t fontAtlasIdx = 0;
	Mesh helloText{};
	JPH::Array<Entity> visibleEntities;

	ScriptRunner* scriptRunner = nullptr;
	FileWatcher* gameplayWatcher = nullptr;
	ArticulationSystem* articulationSystem = nullptr;
	AnimationSystem* animationSystem = nullptr;
	TargetCameraSystem* targetCameraSystem = nullptr;

	TAAState taaState{}; // Kept locally to track sub-pixel jitter indices
};

void DrawConsole(ScriptRunner& runner);
void DrawProfiler(Engine& engine, TAAState& taaState);
void MovementSystem(Engine& engine, float dt);
void AudioSystem(Engine& engine, float dt);
void DrawOrientationGizmo(const ZHLN::Camera& cam);
void DrawInventoryShell(ScriptRunner& runner);

namespace {

// Define the 12 edges connecting the 8 frustum corners
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
// GAME APPLICATION INTERFACE IMPLEMENTATION
// ============================================================================

bool InitializeGame(Engine& engine, GameContext& game) {
	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();
	auto& pc = engine.GetPhysicsContext();
	auto& cam = engine.GetCamera();

	// Allocate a safe temporary state block on the stack to register defaults
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

	// Ensure pure-data handles start initialized to zero
	defaultState.playerPartsCount = 0;
	defaultState.debugLineVbo = 0;

	// Inject the state into the FFI boundary BEFORE loading scripts
	ZHLN_SetGameState(std::bit_cast<ZHLN_Engine*>(&engine), &defaultState);

	reg.RegisterComponents<MeshComponent, PhysicsComponent, MovementComponent,
						   ALife::ALifeComponent, RagdollComponent, NameComponent,
						   TargetCameraComponent>();

	auto groundShape =
		Physics::GetOrCreateShape(pc, Physics::ShapeType::Plane, 0.0f, 1.0f, 0.0f, 0.0f);
	Entity ground = reg.Create();
	reg.Add(ground,
			PhysicsComponent{Physics::CreateRigidBody(
				pc, groundShape, {0, 0, 0}, JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0)});

	game.playerEntity = reg.Create();
	reg.Add(game.playerEntity, MovementComponent{});
	Entity charPhys = Physics::CreateCharacter(pc, JPH::RVec3(0.0f, 3.0f, 0.0f));
	reg.Add(game.playerEntity, PhysicsComponent{charPhys});

	// Instantiate Target Camera entity with initialized post-processing defaults
	Entity cameraEntity = reg.Create();
	reg.Add(cameraEntity, TargetCameraComponent{.target = game.playerEntity,
												.distance = 4.5f,
												.targetDistance = 4.5f,
												.yaw = -90.0f,
												.pitch = -10.0f,
												.stiffness = 15.0f,
												.vignetteIntensity = 1.10f,
												.vignettePower = 1.50f,
												.fov = 45.0f,
												.targetFov = 45.0f});

	game.scriptRunner = new ScriptRunner();
	game.scriptRunner->RunFile("scripts/gameplay.lua");
	game.gameplayWatcher = new FileWatcher("scripts/gameplay.lua");
	game.frameCounter = 0;

	cam.yaw = -90.0f;
	cam.pitch = -10.0f;

	game.fontAtlasIdx = AssetFactory::CreateFontAtlasTexture(rc);
	game.helloText = GUI::CreateTextMesh(rc, "Zahlen Engine - TADC Dorm Showcase", 25.0f, 25.0f,
										 2.5f, {0.9f, 0.1f, 0.1f, 1.0f});

	game.articulationSystem = new ArticulationSystem();
	game.animationSystem = new AnimationSystem();
	game.targetCameraSystem = new TargetCameraSystem();

	return true;
}

static void SetupGameGUI(Engine& engine, GameContext& game, ZHLN_GameState& state) {
	// If we are running in a raw TTY, the entire GUI subsystem is a no-op
	if (engine.GetWindow().IsTTY()) {
		return;
	}

	// 1. Draw Engine and Subshell Terminals
	ZHLN::DrawConsole(*game.scriptRunner);
	ZHLN::DrawInventoryShell(*game.scriptRunner);
	ZHLN::DrawProfiler(engine, game.taaState);
	ZHLN::DrawOrientationGizmo(engine.GetCamera());

	// 2. Main Lighting & Render Controller Panel
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
	if (ImGui::Checkbox("Enable SSR", &useSsr)) {
		state.enableSSR = useSsr ? 1 : 0;
	}

	bool useRtr = state.enableRTR != 0;
	if (ImGui::Checkbox("Enable Hardware RTR", &useRtr)) {
		state.enableRTR = useRtr ? 1 : 0;
	}

	ImGui::End();

	// Flush any modified state parameters back to the shared library memory
	ZHLN_SetGameState(std::bit_cast<ZHLN_Engine*>(&engine), &state);
}

void UpdateGame(Engine& engine, float dt, float& physicsAccumulator, GameContext& game) {
	auto& cam = engine.GetCamera();
	auto& pc = engine.GetPhysicsContext();
	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();

	// Retrieve current global game state (can be updated from Lua scripts)
	ZHLN_GameState state =
		*static_cast<ZHLN_GameState*>(ZHLN_GetGameState(std::bit_cast<ZHLN_Engine*>(&engine)));

	// Update local TAA properties from the shared state before logic & rendering ticks
	game.taaState.enabled = state.enableTAA != 0;
	game.taaState.feedback = state.taaFeedback;

	// Process UI Panels (No-op in TTY)
	SetupGameGUI(engine, game, state);

	// Throttled and compiled-out FileWatcher to completely eliminate once-per-second IO stalls
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

	{
		ZHLN_PROFILE_SCOPE("Logic");

		const float sensitivity = 0.15f;
		if (engine.GetInput().IsMouseButtonDown(KeyCode::RButton)) {
			cam.yaw += engine.GetInput().GetMouse().deltaX * sensitivity;
			cam.pitch = std::clamp(cam.pitch - (engine.GetInput().GetMouse().deltaY * sensitivity),
								   -89.0f, 89.0f);
		}

		game.scriptRunner->CallUpdate(&engine, dt);

		float cappedDt = std::min(dt, 0.1f);
		physicsAccumulator += cappedDt;

		constexpr float targetDt = 1.0f / 60.0f;

		// --- SAFETY: Cap the accumulator to prevent "spiral of death" micro-stutters ---
		physicsAccumulator = std::min(physicsAccumulator, targetDt * 4.0f);

		while (physicsAccumulator >= targetDt) {
			ZHLN::MovementSystem(engine, targetDt);
			pc.Step(targetDt);
			physicsAccumulator -= targetDt;
		}

		game.animationSystem->UpdateAnimations(rc, reg, dt);
		game.articulationSystem->Update(engine, dt);
	}
}

void RenderGame(Engine& engine, float frameTime, float physicsAccumulator, GameContext& game) {
	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();
	auto& pc = engine.GetPhysicsContext();

	// 1. Fetch updated state populated by Lua or ImGui
	ZHLN_GameState state =
		*static_cast<ZHLN_GameState*>(ZHLN_GetGameState(std::bit_cast<ZHLN_Engine*>(&engine)));

	// 2. Extract player parts array from pure data (No shared std::vector!)
	std::vector<Entity> playerParts(state.playerPartsCount);
	for (uint32_t i = 0; i < state.playerPartsCount; ++i) {
		playerParts[i] = Entity::Unpack(state.playerParts[i]);
	}

	auto res = engine.GetWindow().GetSize();
	const auto& worldState = pc.GetWorld();

	// Enforce current active state right before computing projection matrices
	game.taaState.enabled = state.enableTAA != 0;
	game.taaState.feedback = state.taaFeedback;

	if (game.taaState.enabled) {
		game.taaState.frameIndex++;
	} else {
		game.taaState.frameIndex = 0;
	}
	constexpr float targetDt = 1.0f / 60.0f;

	// Fixed: pass primary frameTime directly to prevent drift across multiple Clocks
	game.targetCameraSystem->Update(engine, frameTime, physicsAccumulator / targetDt);

	// Resolve local post-processing settings directly from the active camera component
	float vignetteIntensity = 1.10f;
	float vignettePower = 1.50f;

	auto cameraEntities = reg.GetEntitiesWith<TargetCameraComponent>();
	if (!cameraEntities.empty()) {
		if (auto* camComp = reg.Get<TargetCameraComponent>(cameraEntities[0])) {
			vignetteIntensity = camComp->vignetteIntensity;
			vignettePower = camComp->vignettePower;
		}
	}

	JPH::Mat44 unjitteredProj = cam.GetProjectionMatrix((float)res.width / res.height);
	JPH::Mat44 unjitteredVp = unjitteredProj * cam.GetViewMatrix();

	JPH::Mat44 vp{};
	if (game.taaState.enabled) {
		vp = cam.GetJitteredProjectionMatrix((float)res.width / res.height, res.width, res.height,
											 game.taaState) *
			 cam.GetViewMatrix();
	} else {
		vp = unjitteredVp;
	}

	static JPH::Mat44 s_FrozenVP = JPH::Mat44::sIdentity();
	static std::array<JPH::Vec3, 8> s_FrustumCorners{};
	static bool s_WasFrozen = false;

	if (CullingStats::FreezeFrustum) {
		if (!s_WasFrozen) {
			s_FrozenVP = unjitteredVp;
			JPH::Mat44 invVP = unjitteredVp.Inversed();
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
					s_FrustumCorners[i] =
						JPH::Vec3(worldPos.GetX() / w, worldPos.GetY() / w, worldPos.GetZ() / w);
				}
			}
			s_WasFrozen = true;
		}
		cam.frustum.Update(s_FrozenVP);
	} else {
		cam.frustum.Update(unjitteredVp);
		s_WasFrozen = false;
	}

	CullingSystem<false>(engine, game.visibleEntities, playerParts);

	JPH::Vec3 sunDirection = {0.5f, 1.0f, 0.2f};
	JPH::Mat44 lightView =
		Math::CreateLookAt(sunDirection * 100.0f, {0.0f, 0.0f, 0.0f}, JPH::Vec3::sAxisY());
	JPH::Mat44 lightProj = Math::CreateOrtho(-50.0f, 50.0f, -50.0f, 50.0f, 0.1f, 200.0f);
	JPH::Mat44 shadowProjView = lightProj * lightView;

	JPH::Mat44 biasMatrix = {JPH::Vec4(0.5f, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, -0.5f, 0.0f, 0.0f),
							 JPH::Vec4(0.0f, 0.0f, 1.0f, 0.0f), JPH::Vec4(0.5f, 0.5f, 0.0f, 1.0f)};

	JPH::Mat44 lightSpaceBiased = biasMatrix * shadowProjView;

	static JPH::Mat44 s_PrevUnjitteredVp = unjitteredVp;
	static bool s_FirstFrame = true;
	if (s_FirstFrame) {
		s_PrevUnjitteredVp = unjitteredVp;
		s_FirstFrame = false;
	}

	std::array<GPULight, 3> sceneLights{};

	sceneLights[0].type = 3; // Quad / Area Light
	sceneLights[0].color[0] = 1.0f;
	sceneLights[0].color[1] = 0.8f;
	sceneLights[0].color[2] = 0.6f;
	sceneLights[0].intensity = 5.0f;
	sceneLights[0].twoSided = 0;

	float hX = 2.0f;
	float hZ = 2.0f;
	float lY = 5.0f;

	sceneLights[0].points[0][0] = -hX;
	sceneLights[0].points[0][1] = lY;
	sceneLights[0].points[0][2] = -hZ;

	sceneLights[0].points[1][0] = hX;
	sceneLights[0].points[1][1] = lY;
	sceneLights[0].points[1][2] = -hZ;

	sceneLights[0].points[2][0] = hX;
	sceneLights[0].points[2][1] = lY;
	sceneLights[0].points[2][2] = hZ;

	sceneLights[0].points[3][0] = -hX;
	sceneLights[0].points[3][1] = lY;
	sceneLights[0].points[3][2] = hZ;

	sceneLights[1].position[0] = -5.0f;
	sceneLights[1].position[1] = 4.0f;
	sceneLights[1].position[2] = 0.0f;
	sceneLights[1].type = 1;
	sceneLights[1].color[0] = 0.0f;
	sceneLights[1].color[1] = 0.5f;
	sceneLights[1].color[2] = 1.0f;
	sceneLights[1].intensity = state.light1Intensity;
	sceneLights[1].radius = state.sphereLightRadius;

	sceneLights[2].position[0] = 5.0f;
	sceneLights[2].position[1] = 4.0f;
	sceneLights[2].position[2] = 0.0f;
	sceneLights[2].type = 1;
	sceneLights[2].color[0] = 1.0f;
	sceneLights[2].color[1] = 0.0f;
	sceneLights[2].color[2] = 0.5f;
	sceneLights[2].intensity = state.light2Intensity;
	sceneLights[2].radius = state.sphereLightRadius;

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

	rc.SetTAAState(game.taaState);

	FrameUniforms uniforms{};
	uniforms.viewProj = vp;
	uniforms.unjitteredViewProj = unjitteredVp;
	uniforms.prevUnjitteredViewProj = s_PrevUnjitteredVp;
	uniforms.lightSpaceMatrix = lightSpaceBiased;
	uniforms.invViewProj = unjitteredVp.Inversed();
	std::memcpy(&uniforms.camPos[0], &cam.position, sizeof(float) * 3);
	std::memcpy(&uniforms.lightDir[0], &sunDirection, sizeof(float) * 3);
	uniforms.lightCount = static_cast<uint32_t>(sceneLights.size());
	uniforms.probeMin = JPH::Vec4(state.probeMin[0], state.probeMin[1], state.probeMin[2],
								  state.useLocalProbe ? 1.0f : 0.0f);
	uniforms.probeMax = JPH::Vec4(state.probeMax[0], state.probeMax[1], state.probeMax[2], 0.0f);
	uniforms.probePos = JPH::Vec4(state.probePos[0], state.probePos[1], state.probePos[2], 0.0f);
	uniforms.jitterParams = JPH::Vec4(game.taaState.jitterX, game.taaState.jitterY,
									  game.taaState.prevJitterX, game.taaState.prevJitterY);
	uniforms.enableRTR = state.enableRTR;

	Renderer::SetGISettings(rc, {.mode = state.giMode,
								 .aoRadius = state.aoRadius,
								 .aoBias = state.aoBias,
								 .aoPower = state.aoPower,
								 .giIntensity = state.giIntensity,
								 .giSamples = state.giSamples,
								 .vignetteIntensity = vignetteIntensity, // Dynamic view parameter
								 .vignettePower = vignettePower,		 // Dynamic view parameter
								 .enableSSR = state.enableSSR ? 1 : 0,
								 .enableRTR = state.enableRTR ? 1 : 0});

	Renderer::SetLights(rc, sceneLights.data(), uniforms.lightCount);
	Renderer::SetFrameData(rc, uniforms, shadowProjView);

	Renderer::SetMatrices(rc, vp, unjitteredVp);

	engine.BeginFrame();

	// Read player world matrix (ignoring interpolated translation calculations since they are
	// covered by TargetCameraSystem)
	JPH::Mat44 playerTransform = JPH::Mat44::sIdentity();
	if (reg.IsAlive(game.playerEntity)) {
		bool isRagdollActive = false;
		if (auto* ragComp = reg.Get<RagdollComponent>(game.playerEntity)) {
			isRagdollActive = (ragComp->state == RagdollState::Limp ||
							   ragComp->state == RagdollState::KeyframeMotor);
		}

		if (!isRagdollActive) {
			JPH::Quat rotation = JPH::Quat::sIdentity();
			if (auto* move = reg.Get<MovementComponent>(game.playerEntity)) {
				rotation = JPH::Quat(move->orientation[0], move->orientation[1],
									 move->orientation[2], move->orientation[3]);
			}

			// Simple interpolation for character model visualization
			JPH::Vec3 lerpedPlayerPos = JPH::Vec3::sZero();
			if (auto* phys = reg.Get<PhysicsComponent>(game.playerEntity)) {
				uint32_t dense = worldState.slotToDense[phys->physicsHandle.index];
				const size_t base = static_cast<size_t>(dense) * 4;
				JPH::Vec3 currPos(worldState.positions[base], worldState.positions[base + 1],
								  worldState.positions[base + 2]);
				JPH::Vec3 prevPos(worldState.prevPositions[base],
								  worldState.prevPositions[base + 1],
								  worldState.prevPositions[base + 2]);
				float alpha = std::clamp(physicsAccumulator / targetDt, 0.0f, 1.0f);
				lerpedPlayerPos = prevPos + alpha * (currPos - prevPos);
			}

			playerTransform =
				Math::CreateTransform(lerpedPlayerPos - JPH::Vec3(0.0f, 0.8f, 0.0f), rotation);
		}
	}

	for (Entity e : game.visibleEntities) {
		auto* mesh = reg.Get<MeshComponent>(e);
		if (mesh == nullptr) {
			continue;
		}

		bool isPlayerPart = std::ranges::contains(playerParts, e);
		JPH::Mat44 currentTransform{};
		if (isPlayerPart) {
			currentTransform = playerTransform * mesh->localTransform;
		} else {
			currentTransform = mesh->localTransform;
		}

		DrawFlags flags = DrawFlags::None;
		if (mesh->isSkinned) {
			flags |= DrawFlags::Skinned;
		}
		if (isPlayerPart) {
			flags |= DrawFlags::ExcludeFromTLAS;
		}

		Renderer::Draw(rc, mesh->material, mesh->mesh,
					   {.transform = currentTransform,
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

	if (CullingStats::FreezeFrustum && state.debugLineVbo != 0) {
		Mesh debugMesh = {.vertexBuffer = static_cast<BufferHandle>(state.debugLineVbo),
						  .vertexCount = 36};
		Material debugMat = {.pipeline = static_cast<PipelineHandle>(state.debugLinePipeline),
							 .albedoIndex = state.debugLineAlbedo};

		debugMat.baseColorFactor[0] = 0.0f;
		debugMat.baseColorFactor[1] = 1.0f;
		debugMat.baseColorFactor[2] = 1.0f;
		debugMat.baseColorFactor[3] = 1.0f;

		for (auto s_FrustumEdge : s_FrustumEdges) {
			JPH::Vec3 pA = s_FrustumCorners[s_FrustumEdge.start];
			JPH::Vec3 pB = s_FrustumCorners[s_FrustumEdge.end];

			JPH::Vec3 v = pB - pA;
			float len = v.Length();
			if (len < 1e-4f) {
				continue;
			}

			JPH::Vec3 dir = v / len;
			JPH::Vec3 mid = (pA + pB) * 0.5f;

			JPH::Quat rot = JPH::Quat::sFromTo(JPH::Vec3::sAxisZ(), dir);
			JPH::Mat44 lineTransform = Math::CreateTransform(mid, rot, JPH::Vec3(1.0f, 1.0f, len));

			Renderer::Draw(
				rc, debugMat, debugMesh,
				{.transform = lineTransform, .prevTransform = lineTransform, .cullRadius = len});
		}
	}

	engine.EndFrame();

	// Fixed: pass primary frameTime directly to prevent drift across multiple Clocks
	ZHLN::AudioSystem(engine, frameTime);

	s_PrevUnjitteredVp = unjitteredVp;

	auto allEntities = reg.GetEntitiesWith<MeshComponent>();
	for (Entity e : allEntities) {
		auto* mesh = reg.Get<MeshComponent>(e);
		if (mesh != nullptr) {
			JPH::Mat44 currentTransform{};
			bool isPlayerPart = std::ranges::contains(playerParts, e);

			if (isPlayerPart) {
				currentTransform = playerTransform * mesh->localTransform;
			} else {
				currentTransform = mesh->localTransform;
			}
			mesh->prevTransform = currentTransform;
		}
	}
}

void ShutdownGame([[maybe_unused]] Engine& engine, GameContext& game) {
	// Reclaim heap memories
	delete game.scriptRunner;
	delete game.gameplayWatcher;
	delete game.articulationSystem;
	delete game.animationSystem;
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

	float physicsAccumulator = 0.0f;
	const double targetFrameTime = fpsLimit > 0 ? 1.0 / static_cast<double>(fpsLimit) : 0.0;

	// Reset main loop delta accumulation to discard heavy asset-load / compile startup durations
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

		// Frame Rate Limiter
		if (fpsLimit > 0) {
			auto frameEnd = std::chrono::high_resolution_clock::now();
			double elapsed = std::chrono::duration<double>(frameEnd - frameStart).count();
			if (elapsed < targetFrameTime) {
				double sleepTime = targetFrameTime - elapsed;
				// Yield the thread to the OS if we have a safe margin of time remaining
				if (sleepTime > 0.002) {
					std::this_thread::sleep_for(
						std::chrono::microseconds(static_cast<int64_t>((sleepTime - 0.001) * 1e6)));
				}
				// Spin wait the last 1ms for microsecond accuracy
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
