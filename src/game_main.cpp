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
#include <Zahlen/Scripting.hpp>
#include <algorithm>
#include <cstddef>
#include <detail/ControlFlow.hpp>
#include <engine/system/AnimationSystem.hpp>
#include <engine/system/ArticulationSystem.hpp>
#include <engine/system/CullingSystem.hpp>
#include <physics/PhysicsWorld.hpp>
#include <string>
#include <threading/Mutex.hpp>
#include <threading/TaskSystem.hpp>
#include <vector>

namespace ZHLN {

struct GraphicsSettings {
	int giMode = 1;			  // 0 = Off, 1 = SSAO, 2 = SSGI, 3 = HBAO, 4 = GTAO
	float aoRadius = 0.5f;	  // Radius in meters
	float aoBias = 0.05f;	  // Self-occlusion offset
	float aoPower = 1.8f;	  // SSAO contrast
	float giIntensity = 1.2f; // SSGI bounce strength
	int giSamples = 8;		  // Hemisphere ray samples count
	bool useLocalProbe = true;
	JPH::Vec3 probeMin = JPH::Vec3(-22.0f, 0.0f, -22.0f); // Initial bounding box min (meters)
	JPH::Vec3 probeMax = JPH::Vec3(22.0f, 12.0f, 22.0f);  // Initial bounding box max (meters)
	JPH::Vec3 probePos = JPH::Vec3(0.0f, 4.0f, 0.0f); // Room center (where cubemap was captured)
	float vignetteIntensity = 1.10f;				  // 0.0f represents completely disabled
	float vignettePower = 1.50f;					  // Softness falloff exponent
	bool enableSSR = true;
	TAAState taaState{};
};

struct WorkspaceSettings {
	float floorRoughness = 0.15f;
	float floorMetallic = 0.95f;
	float floorYOffset = 0.72f;
	float sphereLightRadius = 1.5f;
	float light1Intensity = 180.0f;
	float light2Intensity = 180.0f;
};

struct SceneContext {
	std::vector<Entity> pomniParts;
	Entity visualFloorEntity = NullEntity;
	Material floorMaterial{};
	Mesh debugLineMesh{};
	Material debugLineMaterial{};

	void Setup(Engine& engine, RenderContext& rc, ECS::Registry& reg, PhysicsContext& pc) {
		ZHLN::Log("Assembling Scene with Pure Runtime glTF Parsing...");

		auto* lobbyPrefab =
			AssetFactory::LoadModelPrefab(rc, engine.GetAssetManager(), "Circus Lobby V9.glb");
		if (lobbyPrefab != nullptr) {
			AssetFactory::SpawnParams params;
			params.createPhysics = true;
			params.useBoxColliders = false; // <-- CRITICAL: Must be false (mesh shape) so player is
											// not stuck in a solid box
			params.isStaticPhysics = true;
			AssetFactory::InstantiatePrefab(rc, reg, pc, *lobbyPrefab, params);
		}

		auto* pomniPrefab =
			AssetFactory::LoadModelPrefab(rc, engine.GetAssetManager(), "tadc_models/POMNI.glb");
		if (pomniPrefab != nullptr) {
			AssetFactory::SpawnParams params;
			params.createPhysics = false;
			params.isAnimated = true;

			pomniParts.resize(128); // Pre-allocate ample buffer
			uint32_t pomniCount = AssetFactory::InstantiatePrefab(
				rc, reg, pc, *pomniPrefab, params, pomniParts.data(), pomniParts.size());
			pomniParts.resize(pomniCount);
		}
	}

	void FindFloorEntities(ECS::Registry& reg) {
		auto entities = reg.GetEntitiesWith<NameComponent>();
		auto names = reg.GetRawArray<NameComponent>();

		for (size_t i = 0; i < entities.size(); ++i) {
			std::string nameLower(names[i].name.c_str());
			std::ranges::transform(nameLower, nameLower.begin(), ::tolower);

			// Scan for keywords commonly exported by Blender/glTF for floor meshes
			if (nameLower.contains("floor") || nameLower.contains("ground") ||
				nameLower.contains("lobby")) {
				// Ensure it actually carries visual geometry
				if (reg.Get<MeshComponent>(entities[i]) != nullptr) {
					visualFloorEntity = entities[i];
					break; // Assign primary visual floor
				}
			}
		}
	}
};

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

	GraphicsSettings graphics{};
	WorkspaceSettings workspace{};
	SceneContext scene{};
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

	// 1. Register Native components
	reg.RegisterComponents<MeshComponent, PhysicsComponent, MovementComponent,
						   ALife::ALifeComponent, RagdollComponent, NameComponent>();

	// 2. Create Physical Ground Plane
	auto groundShape =
		Physics::GetOrCreateShape(pc, Physics::ShapeType::Plane, 0.0f, 1.0f, 0.0f, 0.0f);
	Entity ground = reg.Create();
	reg.Add(ground,
			PhysicsComponent{Physics::CreateRigidBody(
				pc, groundShape, {0, 0, 0}, JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0)});

	// 3. Create the Player Entity with Character Controller
	game.playerEntity = reg.Create();
	reg.Add(game.playerEntity, MovementComponent{});
	Entity charPhys = Physics::CreateCharacter(pc, JPH::RVec3(0.0f, 3.0f, 0.0f));
	reg.Add(game.playerEntity, PhysicsComponent{charPhys});

	// 4. Initialize Scripting Subsystem
	game.scriptRunner = new ScriptRunner();
	game.scriptRunner->RunFile("scripts/gameplay.lua");
	game.gameplayWatcher = new FileWatcher("scripts/gameplay.lua");
	game.frameCounter = 0;

	// 5. Spawn Level Prefabs (Circus Lobby V9.glb)
	game.scene.Setup(engine, rc, reg, pc);

	// --- 5.1 QUERY THE SPAWNED ENVIRONMENT FOR THE FLOOR ENTITY ---
	game.scene.FindFloorEntities(reg);
	if (game.scene.visualFloorEntity != NullEntity) {
		ZHLN::Log("[DOD Query] Successfully binded to glTF Floor Entity! Handle Index: {}",
				  game.scene.visualFloorEntity.index);
	} else {
		ZHLN::Log("[DOD Query] WARNING: Could not find floor mesh in glTF scene.");
	}

	AssetFactory::SetupPlayerRagdoll(rc, pc, reg, game.playerEntity, game.scene.pomniParts);

	// Position Camera orientation initially looking forward
	cam.yaw = -90.0f;
	cam.pitch = -10.0f;

	game.fontAtlasIdx = AssetFactory::CreateFontAtlasTexture(rc);
	game.helloText = GUI::CreateTextMesh(rc, "Zahlen Engine - TADC Dorm Showcase", 25.0f, 25.0f,
										 2.5f, {0.9f, 0.1f, 0.1f, 1.0f});

	game.scene.debugLineMesh = AssetFactory::CreateBox(rc, {0.02f, 0.02f, 0.5f});
	game.scene.debugLineMaterial = AssetFactory::CreateBasicMaterial(rc);
	game.scene.debugLineMaterial.baseColorFactor[0] = 0.0f; // Neon Cyan
	game.scene.debugLineMaterial.baseColorFactor[1] = 1.0f;
	game.scene.debugLineMaterial.baseColorFactor[2] = 1.0f;
	game.scene.debugLineMaterial.baseColorFactor[3] = 1.0f;

	game.articulationSystem = new ArticulationSystem();
	game.animationSystem = new AnimationSystem();

	return true;
}

void RenderGame(Engine& engine, float physicsAccumulator, GameContext& game) {

	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();
	auto& pc = engine.GetPhysicsContext();

	// Static local clock to calculate independent frameTime delta for audio threading [1]
	static Clock deltaClock;
	float renderFrameTime = deltaClock.GetDeltaTime();

	auto res = engine.GetWindow().GetSize();
	const auto& worldState = pc.GetWorld();

	if (game.graphics.taaState.enabled) {
		game.graphics.taaState.frameIndex++;
	} else {
		game.graphics.taaState.frameIndex = 0;
	}

	// 1. Retrieve & Interpolate Player Position from Jolt [6]
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

	// 2. Camera Placement
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
	if (game.graphics.taaState.enabled) {
		vp = cam.GetJitteredProjectionMatrix((float)res.width / res.height, res.width, res.height,
											 game.graphics.taaState) *
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

	CullingSystem<false>(engine, game.visibleEntities, game.scene.pomniParts);

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

	// 3. Compile lights list & sync material factors [1]
	std::array<GPULight, 3> sceneLights{};

	// --- NEW: GIANT RECTANGULAR PANEL LIGHT (LTC) ---
	sceneLights[0].type = 3; // Quad / Area Light
	sceneLights[0].color[0] = 1.0f;
	sceneLights[0].color[1] = 0.8f;
	sceneLights[0].color[2] = 0.6f; // Warm tungsten color
	sceneLights[0].intensity = 5.0f;
	sceneLights[0].twoSided = 0; // Only emits light downwards

	// Define the 4 corners of a 4x4 meter ceiling panel
	float hX = 2.0f;
	float hZ = 2.0f;
	float lY = 5.0f; // 5 meters up in the air

	// Bottom-Left
	sceneLights[0].points[0][0] = -hX;
	sceneLights[0].points[0][1] = lY;
	sceneLights[0].points[0][2] = -hZ;

	// Bottom-Right
	sceneLights[0].points[1][0] = hX;
	sceneLights[0].points[1][1] = lY; // <-- FIX: Was sceneLights[1]
	sceneLights[0].points[1][2] = -hZ;

	// Top-Right
	sceneLights[0].points[2][0] = hX;
	sceneLights[0].points[2][1] = lY;
	sceneLights[0].points[2][2] = hZ;

	// Top-Left
	sceneLights[0].points[3][0] = -hX;
	sceneLights[0].points[3][1] = lY;
	sceneLights[0].points[3][2] = hZ;

	// --- LIGHT 1: CYAN SPHERE ---
	sceneLights[1].position[0] = -5.0f; // <-- FIX: Was sceneLights[0]
	sceneLights[1].position[1] = 4.0f;
	sceneLights[1].position[2] = 0.0f;
	sceneLights[1].type = 1;
	sceneLights[1].color[0] = 0.0f;
	sceneLights[1].color[1] = 0.5f;
	sceneLights[1].color[2] = 1.0f;
	sceneLights[1].intensity = game.workspace.light1Intensity;
	sceneLights[1].radius = game.workspace.sphereLightRadius;

	// --- LIGHT 2: MAGENTA SPHERE ---
	sceneLights[2].position[0] = 5.0f; // <-- FIX: Was sceneLights[1]
	sceneLights[2].position[1] = 4.0f;
	sceneLights[2].position[2] = 0.0f;
	sceneLights[2].type = 1;
	sceneLights[2].color[0] = 1.0f;
	sceneLights[2].color[1] = 0.0f;
	sceneLights[2].color[2] = 0.5f;
	sceneLights[2].intensity = game.workspace.light2Intensity;
	sceneLights[2].radius = game.workspace.sphereLightRadius;

	// --- ALL-FLOOR DYNAMIC UPDATE FIX ---
	// Traverse the registry to update every floor, ground, or lobby primitive cleanly
	auto floorEntities = reg.GetEntitiesWith<NameComponent>();
	auto floorNames = reg.GetRawArray<NameComponent>();

	for (size_t i = 0; i < floorEntities.size(); ++i) {
		std::string nameLower(floorNames[i].name.c_str());
		std::ranges::transform(nameLower, nameLower.begin(), ::tolower);

		if (nameLower.contains("floor") || nameLower.contains("ground") ||
			nameLower.contains("lobby")) {
			if (auto* floorMeshComp = reg.Get<MeshComponent>(floorEntities[i])) {
				floorMeshComp->material.roughnessFactor = game.workspace.floorRoughness;
				floorMeshComp->material.metallicFactor = game.workspace.floorMetallic;
			}
		}
	}

	rc.SetTAAState(game.graphics.taaState);

	FrameUniforms uniforms{};
	uniforms.viewProj = vp;
	uniforms.unjitteredViewProj = unjitteredVp;
	uniforms.prevUnjitteredViewProj = s_PrevUnjitteredVp;
	uniforms.lightSpaceMatrix = lightSpaceBiased;
	uniforms.invViewProj = unjitteredVp.Inversed();
	std::memcpy(&uniforms.camPos[0], &cam.position, sizeof(float) * 3);
	std::memcpy(&uniforms.lightDir[0], &sunDirection, sizeof(float) * 3);
	uniforms.lightCount = static_cast<uint32_t>(sceneLights.size());
	uniforms.probeMin =
		JPH::Vec4(game.graphics.probeMin, game.graphics.useLocalProbe ? 1.0f : 0.0f);
	uniforms.probeMax = JPH::Vec4(game.graphics.probeMax, 0.0f);
	uniforms.probePos = JPH::Vec4(game.graphics.probePos, 0.0f);
	uniforms.jitterParams =
		JPH::Vec4(game.graphics.taaState.jitterX, game.graphics.taaState.jitterY,
				  game.graphics.taaState.prevJitterX, game.graphics.taaState.prevJitterY);

	Renderer::SetGISettings(rc, {.mode = game.graphics.giMode,
								 .aoRadius = game.graphics.aoRadius,
								 .aoBias = game.graphics.aoBias,
								 .aoPower = game.graphics.aoPower,
								 .giIntensity = game.graphics.giIntensity,
								 .giSamples = game.graphics.giSamples,
								 .vignetteIntensity = game.graphics.vignetteIntensity,
								 .vignettePower = game.graphics.vignettePower,
								 .enableSSR = game.graphics.enableSSR ? 1 : 0});

	Renderer::SetLights(rc, sceneLights.data(), uniforms.lightCount); // Upload SSBO to GPU
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

		bool isPlayerPart = std::ranges::contains(game.scene.pomniParts, e);
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

	// --- DRAW THE FREEZE WIREFRAME FRUSTUM ---
	if (CullingStats::FreezeFrustum &&
		game.scene.debugLineMesh.vertexBuffer != BufferHandle::Invalid) {
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

			Renderer::Draw(rc, game.scene.debugLineMaterial, game.scene.debugLineMesh,
						   lineTransform, lineTransform, len);
		}
	}

	engine.EndFrame();

	ZHLN::AudioSystem(engine, renderFrameTime); // Decoupled frametime for precise audio pacing [1]

	s_PrevUnjitteredVp = unjitteredVp;

	// Global update for prevTransforms
	auto allEntities = reg.GetEntitiesWith<MeshComponent>();
	for (Entity e : allEntities) {
		auto* mesh = reg.Get<MeshComponent>(e);
		if (mesh != nullptr) {
			JPH::Mat44 currentTransform{};
			bool isPlayerPart = std::ranges::contains(game.scene.pomniParts, e);

			if (isPlayerPart) {
				currentTransform = playerTransform * mesh->localTransform;
			} else {
				currentTransform = mesh->localTransform;
			}
			mesh->prevTransform = currentTransform;
		}
	}
}

void UpdateGame(Engine& engine, float dt, float& physicsAccumulator, GameContext& game) {
	auto& cam = engine.GetCamera();
	auto& pc = engine.GetPhysicsContext();
	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();

	// 1. Draw HUD Layouts
	ZHLN::DrawConsole(*game.scriptRunner);
	ZHLN::DrawInventoryShell(*game.scriptRunner);
	ZHLN::DrawProfiler(engine, game.graphics.taaState);
	ZHLN::DrawOrientationGizmo(cam);

	// 2. Lighting Workspace Developer Menu
	ImGui::Begin("Lighting Workspace Controller");
	ImGui::Text("Specular Mips & Area Lights Debugger");
	ImGui::Separator();
	ImGui::SliderFloat("Sphere Light Radius", &game.workspace.sphereLightRadius, 0.0f, 5.0f);
	ImGui::SliderFloat("Cyan Intensity", &game.workspace.light1Intensity, 0.0f, 500.0f);
	ImGui::SliderFloat("Magenta Intensity", &game.workspace.light2Intensity, 0.0f, 500.0f);
	ImGui::Separator();
	ImGui::SliderFloat("Floor Roughness", &game.workspace.floorRoughness, 0.0f, 1.0f);
	ImGui::SliderFloat("Floor Metallic", &game.workspace.floorMetallic, 0.0f, 1.0f);

	// --- PARALLAX-CORRECTED PROBE CONTROLS ---
	ImGui::SeparatorText("Parallax-Corrected Local Reflection Probe");
	ImGui::Checkbox("Enable Box Projection", &game.graphics.useLocalProbe);
	if (game.graphics.useLocalProbe) {
		std::array<float, 3> minArr = {game.graphics.probeMin.GetX(), game.graphics.probeMin.GetY(),
									   game.graphics.probeMin.GetZ()};
		std::array<float, 3> maxArr = {game.graphics.probeMax.GetX(), game.graphics.probeMax.GetY(),
									   game.graphics.probeMax.GetZ()};
		std::array<float, 3> posArr = {game.graphics.probePos.GetX(), game.graphics.probePos.GetY(),
									   game.graphics.probePos.GetZ()};

		if (ImGui::DragFloat3("Box Min", minArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
			game.graphics.probeMin = JPH::Vec3(minArr[0], minArr[1], minArr[2]);
		}
		if (ImGui::DragFloat3("Box Max", maxArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
			game.graphics.probeMax = JPH::Vec3(maxArr[0], maxArr[1], maxArr[2]);
		}
		if (ImGui::DragFloat3("Probe Position", posArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
			game.graphics.probePos = JPH::Vec3(posArr[0], posArr[1], posArr[2]);
		}
	}

	// --- COOPERATIVE SSAO / SSGI CONTROLLER ---
	ImGui::SeparatorText("Ambient Occlusion & Global Illumination");
	const char* giModesList[] = {"Off", "SSAO (Ambient Occlusion)", "SSGI (Screen Space GI)",
								 "HBAO (Horizon-Based AO)", "GTAO (Ground Truth AO)"};
	ImGui::Combo("GI Mode", &game.graphics.giMode, giModesList, IM_ARRAYSIZE(giModesList));

	if (game.graphics.giMode == 1) { // SSAO Settings
		ImGui::SliderFloat("AO Radius", &game.graphics.aoRadius, 0.05f, 2.5f, "%.2fm");
		ImGui::SliderFloat("AO Bias", &game.graphics.aoBias, 0.001f, 0.2f, "%.3f");
		ImGui::SliderFloat("AO Contrast", &game.graphics.aoPower, 0.5f, 5.0f, "%.1fx");
		ImGui::SliderInt("AO Samples", &game.graphics.giSamples, 2, 32);
	} else if (game.graphics.giMode == 2) { // SSGI Settings
		ImGui::SliderFloat("Bounce Radius", &game.graphics.aoRadius, 0.05f, 2.5f, "%.2fm");
		ImGui::SliderFloat("Bounce Bias", &game.graphics.aoBias, 0.001f, 0.2f, "%.3f");
		ImGui::SliderFloat("GI Bounce Intensity", &game.graphics.giIntensity, 0.1f, 5.0f, "%.1fx");
		ImGui::SliderInt("GI Samples", &game.graphics.giSamples, 2, 32);
	} else if (game.graphics.giMode == 3 || game.graphics.giMode == 4) { // HBAO / GTAO Settings
		ImGui::SliderFloat("Search Radius", &game.graphics.aoRadius, 0.05f, 3.0f, "%.2fm");
		ImGui::SliderFloat("Acne Bias", &game.graphics.aoBias, 0.001f, 0.2f, "%.3f");
		ImGui::SliderFloat("Shadow Contrast", &game.graphics.aoPower, 0.5f, 6.0f, "%.1fx");
		ImGui::SliderInt("Search Steps", &game.graphics.giSamples, 4,
						 32); // Represents total step count
	}

	// --- CAMERA VIGNETTE ---
	ImGui::Separator();
	ImGui::SeparatorText("Camera Vignette");
	ImGui::SliderFloat("Vignette Intensity", &game.graphics.vignetteIntensity, 0.0f, 2.5f, "%.2f");
	if (game.graphics.vignetteIntensity > 0.0f) {
		ImGui::SliderFloat("Vignette Power", &game.graphics.vignettePower, 0.1f, 6.0f, "%.2f");
	}

	ImGui::Checkbox("Enable SSR", &game.graphics.enableSSR);

	ImGui::End();

	// 3. Hot Reload Script Files
	if (++game.frameCounter % 60 == 0 && game.gameplayWatcher->CheckModified()) {
		game.scriptRunner->ReloadFile("scripts/gameplay.lua");
	}

	{
		ZHLN_PROFILE_SCOPE("Logic");

		// Mouse look (Active only when holding Right Click)
		const float sensitivity = 0.15f;
		if (engine.GetInput().IsMouseButtonDown(KeyCode::RButton)) {
			cam.yaw += engine.GetInput().GetMouse().deltaX * sensitivity;
			cam.pitch = std::clamp(cam.pitch - (engine.GetInput().GetMouse().deltaY * sensitivity),
								   -89.0f, 89.0f);
		}

		game.scriptRunner->CallUpdate(&engine, dt);

		// 4. Physics Tick loop
		physicsAccumulator += dt;
		constexpr float targetDt = 1.0f / 60.0f;
		while (physicsAccumulator >= targetDt) {
			ZHLN::MovementSystem(engine, targetDt);
			pc.Step(targetDt);
			physicsAccumulator -= targetDt;
		}

		// 5. Run animation systems
		game.animationSystem->UpdateAnimations(rc, reg, dt);
		game.articulationSystem->Update(engine, dt);
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
// CLEAN ENTRYPOINT ENGINE CONTROL LOOP [1]
// ============================================================================

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
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
