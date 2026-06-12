// src/game_main.cpp

#include "Zahlen/Input.hpp"
#include "Zahlen/alife/Types.hpp"
#include "ecs/ECS.hpp"
#include "engine/FileWatcher.hpp"
#include "engine/Platform.hpp"
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
#include <cstddef>
#include <detail/ControlFlow.hpp>
#include <engine/system/AnimationSystem.hpp>
#include <engine/system/ArticulationSystem.hpp>
#include <engine/system/CullingSystem.hpp>
#include <physics/PhysicsWorld.hpp>
#include <print>
#include <string>
#include <threading/Mutex.hpp>
#include <threading/TaskSystem.hpp>
#include <vector>

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
	ZHLN_SetGameState(reinterpret_cast<ZHLN_Engine*>(&engine), &defaultState);

	reg.RegisterComponents<MeshComponent, PhysicsComponent, MovementComponent,
						   ALife::ALifeComponent, RagdollComponent, NameComponent>();

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

	return true;
}

void UpdateGame(Engine& engine, float dt, float& physicsAccumulator, GameContext& game) {
	auto& cam = engine.GetCamera();
	auto& pc = engine.GetPhysicsContext();
	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();

	ZHLN_GameState state =
		*static_cast<ZHLN_GameState*>(ZHLN_GetGameState(reinterpret_cast<ZHLN_Engine*>(&engine)));

	// Update local TAA properties from the shared state before UI / Profiler ticks
	game.taaState.enabled = state.enableTAA != 0;
	game.taaState.feedback = state.taaFeedback;

	ZHLN::DrawConsole(*game.scriptRunner);
	ZHLN::DrawInventoryShell(*game.scriptRunner);
	ZHLN::DrawProfiler(engine, game.taaState);
	ZHLN::DrawOrientationGizmo(cam);

	// Synchronize any updates made by DrawProfiler back to the shared state
	state.enableTAA = game.taaState.enabled ? 1 : 0;
	state.taaFeedback = game.taaState.feedback;

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

	ImGui::End();

	// Flush modified stack parameters back to the shared library memory
	ZHLN_SetGameState(reinterpret_cast<ZHLN_Engine*>(&engine), &state);

	if (++game.frameCounter % 60 == 0 && game.gameplayWatcher->CheckModified()) {
		game.scriptRunner->ReloadFile("scripts/gameplay.lua");
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

		physicsAccumulator += dt;
		constexpr float targetDt = 1.0f / 60.0f;
		while (physicsAccumulator >= targetDt) {
			ZHLN::MovementSystem(engine, targetDt);
			pc.Step(targetDt);
			physicsAccumulator -= targetDt;
		}

		game.animationSystem->UpdateAnimations(rc, reg, dt);
		game.articulationSystem->Update(engine, dt);
	}
}

void RenderGame(Engine& engine, float physicsAccumulator, GameContext& game) {
	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();
	auto& pc = engine.GetPhysicsContext();

	// 1. Fetch updated state populated by Lua or ImGui
	ZHLN_GameState state =
		*static_cast<ZHLN_GameState*>(ZHLN_GetGameState(reinterpret_cast<ZHLN_Engine*>(&engine)));

	// 2. Extract player parts array from pure data (No shared std::vector!)
	std::vector<Entity> playerParts(state.playerPartsCount);
	for (uint32_t i = 0; i < state.playerPartsCount; ++i) {
		playerParts[i] = Entity::Unpack(state.playerParts[i]);
	}

	static Clock deltaClock;
	float renderFrameTime = deltaClock.GetDeltaTime();

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

	JPH::Vec3 playerPos = JPH::Vec3::sZero();
	constexpr float targetDt = 1.0f / 60.0f;
	if (reg.IsAlive(game.playerEntity)) {
		if (auto* phys = reg.Get<PhysicsComponent>(game.playerEntity)) {
			uint32_t dense = worldState.slotToDense[phys->physicsHandle.index];
			const size_t base = static_cast<size_t>(dense) * 4;

			JPH::Vec3 currPos((float)worldState.positions[base],
							  (float)worldState.positions[base + 1],
							  (float)worldState.positions[base + 2]);

			JPH::Vec3 prevPos((float)worldState.prevPositions[base],
							  (float)worldState.prevPositions[base + 1],
							  (float)worldState.prevPositions[base + 2]);

			float alpha = std::clamp(physicsAccumulator / targetDt, 0.0f, 1.0f);
			playerPos = prevPos + alpha * (currPos - prevPos);
		}
	}

	float yawRad = JPH::DegreesToRadians(cam.yaw);
	float pitchRad = JPH::DegreesToRadians(cam.pitch);
	JPH::Vec3 offsetDir(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad),
						JPH::Sin(yawRad) * JPH::Cos(pitchRad));

	float wheelDelta = engine.GetInput().GetMouse().wheel;
	if (std::abs(wheelDelta) > 0.01f) {
		game.camDistance = JPH::Clamp(game.camDistance - wheelDelta * 0.5f, 1.5f, 15.0f);
	}
	cam.position =
		playerPos - (offsetDir.Normalized() * game.camDistance) + JPH::Vec3(0.0f, 1.3f, 0.0f);

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

	Renderer::SetGISettings(rc, {.mode = state.giMode,
								 .aoRadius = state.aoRadius,
								 .aoBias = state.aoBias,
								 .aoPower = state.aoPower,
								 .giIntensity = state.giIntensity,
								 .giSamples = state.giSamples,
								 .vignetteIntensity = state.vignetteIntensity,
								 .vignettePower = state.vignettePower,
								 .enableSSR = state.enableSSR ? 1 : 0});

	Renderer::SetLights(rc, sceneLights.data(), uniforms.lightCount);
	Renderer::SetFrameData(rc, uniforms, shadowProjView);

	Renderer::SetMatrices(rc, vp, unjitteredVp);

	engine.BeginFrame();

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
			playerTransform =
				Math::CreateTransform(playerPos - JPH::Vec3(0.0f, 0.8f, 0.0f), rotation);
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

		Renderer::Draw(rc, mesh->material, mesh->mesh, currentTransform, mesh->prevTransform,
					   mesh->cullRadius, mesh->jointOffset, mesh->isSkinned, mesh->morphOffset,
					   mesh->activeMorphCount, mesh->morphWeights);
	}

	CullingStats::TotalObjects = (uint32_t)reg.GetEntitiesWith<MeshComponent>().size();
	CullingStats::CulledObjects =
		CullingStats::TotalObjects - (uint32_t)game.visibleEntities.size();

	Renderer::DrawUI(rc, game.helloText, game.fontAtlasIdx);

	// Retrieve debug line properties directly from the shared POD struct (no functions required!)
	if (CullingStats::FreezeFrustum && state.debugLineVbo != 0) {
		Mesh debugMesh = {.vertexBuffer = static_cast<BufferHandle>(state.debugLineVbo),
						  .vertexCount = 36};
		Material debugMat = {.pipeline = static_cast<PipelineHandle>(state.debugLinePipeline),
							 .albedoIndex = state.debugLineAlbedo};

		// Setup the neon color locally
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

			Renderer::Draw(rc, debugMat, debugMesh, lineTransform, lineTransform, len);
		}
	}

	engine.EndFrame();

	ZHLN::AudioSystem(engine, renderFrameTime);

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

// ============================================================================
// CLEAN ENTRYPOINT ENGINE CONTROL LOOP
// ============================================================================

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
	for (int i = 1; i < argc; ++i) {
		if (std::string_view(argv[i]) == "--version") {
// Compile-time compiler detection
#if defined(__clang__)
			constexpr std::string_view compiler = "Clang (" __VERSION__ ")";
#elif defined(__GNUC__)
			constexpr std::string_view compiler = "GCC (" __VERSION__ ")";
#elif defined(_MSC_VER)
			constexpr std::string_view compiler = "MSVC";
#else
			constexpr std::string_view compiler = "Unknown Compiler";
#endif

// Compile-time build configuration detection
#if defined(NDEBUG)
			constexpr std::string_view buildType = "Release";
#else
			constexpr std::string_view buildType = "Debug";
#endif

// Compile-time sanitizer detection
#if defined(__ASAN_ENABLED__)
			constexpr std::string_view sanitizers = "enabled";
#else
			constexpr std::string_view sanitizers = "disabled";
#endif

			std::println("Zahlen Engine - version 1.0.0");
			std::println("Built on:     {} (UTC)", __DATE__);
			std::println("Build Profile: {} | Sanitizers: {}", buildType, sanitizers);
			std::println("Compiler:      {}", compiler);
			std::println("\nThis is free software; see the source for copying conditions.");
			std::println("There is NO warranty; not even for MERCHANTABILITY or FITNESS FOR A "
						 "PARTICULAR PURPOSE.");
			return EXIT_SUCCESS;
		}
	}
	Platform::Init();
	ZHLN::SetupSignalHandler();
	TaskSystem::Init();
	Clock clock;

	ZHLN::EngineConfig config{
		.physics = {.maxBodies = 5000,
					.maxBodyPairs = 10000,
					.maxContactConstraints = 10000,
					.tempAllocatorSize = 64 * 1024 * 1024},
		.render = {.appName = "Zahlen Engine - Digital Circus Showcase",
				   .width = 1280,
				   .height = 720,
				   .vsync = false,
				   .enableValidation = true},
	};

	{
		Engine engine(config);
		engine.GetWindow().Focus();

		// Local stack allocation of context cleanly separates game loop dependencies
		GameContext game{};

		if (!ZHLN::InitializeGame(engine, game)) {
			ZHLN::Log("Fatal: Game failed to initialize.");
			return EXIT_FAILURE;
		}

		float physicsAccumulator = 0.0f;

		while (engine.IsRunning()) {
			float frameTime = clock.GetDeltaTime();
			engine.ProcessEvents();

			if (engine.GetInput().IsKeyDown(KeyCode::Escape)) {
				engine.GetWindow().Close();
				break;
			}

			auto res = engine.GetWindow().GetSize();
			if (res.width <= 0 || res.height <= 0) {
				Platform::Sleep(10);
				continue;
			}

			if (engine.GetInput().NeedsResize()) {
				engine.GetRenderContext().SetResolution(engine.GetInput().GetNewSize());
				engine.GetInput().ClearResizeFlag();
				continue;
			}

			// Execute game simulation ticks and rendering (BeginFrame inside RenderGame)
			// 1. Update physics, gameplay logic, and record ImGui windows
			ZHLN::UpdateGame(engine, frameTime, physicsAccumulator, game);

			// 2. Render scene, overlay ImGui draw data, and submit to GPU
			ZHLN::RenderGame(engine, physicsAccumulator, game);
		}

		ZHLN::ShutdownGame(engine, game);
	}

	TaskSystem::Shutdown();
	return EXIT_SUCCESS;
}
