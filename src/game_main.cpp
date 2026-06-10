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
void DrawConsole(ScriptRunner& runner);
void DrawProfiler(Engine& engine, TAAState& taaState);
void MovementSystem(Engine& engine, float dt);
void AudioSystem(Engine& engine, float dt);
void DrawOrientationGizmo(const ZHLN::Camera& cam);
void DrawInventoryShell(ScriptRunner& runner);

// Define the 12 edges connecting the 8 frustum corners
struct FrustumEdge {
	int start;
	int end;
};

// Decoupled application loop layers
bool InitializeGame(Engine& engine);
void UpdateGame(Engine& engine, float dt, float& physicsAccumulator);
void RenderGame(Engine& engine, float physicsAccumulator);
void ShutdownGame(Engine& engine);
} // namespace ZHLN

using namespace ZHLN;

namespace {

// Anonymous namespace for file-scoped static state (id-style memory isolation) [1]
static std::vector<Entity> s_PomniParts;
static Entity s_PlayerEntity = NullEntity;
static float s_CamDistance = 4.5f;
static int s_GIMode = 1;		   // 0 = Off, 1 = SSAO, 2 = SSGI
static float s_AORadius = 0.5f;	   // Radius in meters
static float s_AOBias = 0.05f;	   // Self-occlusion offset
static float s_AOPower = 1.8f;	   // SSAO contrast
static float s_GIIntensity = 1.2f; // SSGI bounce strength
static int s_GISamples = 8;		   // Hemisphere ray samples count
static bool s_UseLocalProbe = true;
static JPH::Vec3 s_ProbeMin = JPH::Vec3(-22.0f, 0.0f, -22.0f); // Initial bounding box min (meters)
static JPH::Vec3 s_ProbeMax = JPH::Vec3(22.0f, 12.0f, 22.0f);  // Initial bounding box max (meters)
static JPH::Vec3 s_ProbePos =
	JPH::Vec3(0.0f, 4.0f, 0.0f);		  // Room center (where cubemap was captured)
static float s_VignetteIntensity = 1.10f; // 0.0f represents completely disabled
static float s_VignettePower = 1.50f;	  // Softness falloff exponent
static bool s_EnableSSR = true;

static TAAState s_TAAState;

static constexpr FrustumEdge s_FrustumEdges[12] = {
	{.start = 0, .end = 1}, {.start = 1, .end = 2},
	{.start = 2, .end = 3}, {.start = 3, .end = 0}, // Near Plane loop
	{.start = 4, .end = 5}, {.start = 5, .end = 6},
	{.start = 6, .end = 7}, {.start = 7, .end = 4}, // Far Plane loop
	{.start = 0, .end = 4}, {.start = 1, .end = 5},
	{.start = 2, .end = 6}, {.start = 3, .end = 7} // Near-to-Far connection lines
};

// Visual / Debug Mesh allocations
static Mesh s_DebugLineMesh = {};
static Material s_DebugLineMaterial = {};

// Interactive Lighting Workspace parameters
static Entity s_VisualFloorEntity = NullEntity;
static float s_FloorRoughness = 0.15f;
static float s_FloorMetallic = 0.95f;
static float s_FloorYOffset = 0.72f;
static Material s_FloorMaterial{};

static float s_SphereLightRadius = 1.5f;
static float s_Light1Intensity = 180.0f;
static float s_Light2Intensity = 180.0f;

// Engine subsystems pointers
static ScriptRunner* s_ScriptRunner = nullptr;
static FileWatcher* s_GameplayWatcher = nullptr;
static ArticulationSystem* s_ArticulationSystem = nullptr;
static AnimationSystem* s_AnimationSystem = nullptr;

static uint32_t s_FrameCounter = 0;
static uint32_t s_FontAtlasIdx = 0;
static Mesh s_HelloText = {};

struct Scene {
	void Setup(Engine& engine) {
		auto& rc = engine.GetRenderContext();
		auto& reg = engine.GetRegistry();

		ZHLN::Log("Assembling Scene with Pure Runtime glTF Parsing...");

		auto* lobbyPrefab =
			AssetFactory::LoadModelPrefab(rc, engine.GetAssetManager(), "Circus Lobby V9.glb");
		if (lobbyPrefab != nullptr) {
			AssetFactory::SpawnParams params;
			params.createPhysics = true;
			params.useBoxColliders = false; // <-- CRITICAL: Must be false (mesh shape) so player is
											// not stuck in a solid box
			params.isStaticPhysics = true;
			AssetFactory::InstantiatePrefab(rc, reg, engine.GetPhysicsContext(), *lobbyPrefab,
											params);
		}

		auto* pomniPrefab =
			AssetFactory::LoadModelPrefab(rc, engine.GetAssetManager(), "tadc_models/POMNI.glb");
		if (pomniPrefab != nullptr) {
			AssetFactory::SpawnParams params;
			params.createPhysics = false;
			params.isAnimated = true;

			s_PomniParts.resize(128); // Pre-allocate ample buffer
			uint32_t pomniCount = AssetFactory::InstantiatePrefab(
				rc, reg, engine.GetPhysicsContext(), *pomniPrefab, params, s_PomniParts.data(),
				(uint32_t)s_PomniParts.size());
			s_PomniParts.resize(pomniCount);
		}
	}
};

JPH::Array<ZHLN::Entity> s_VisibleEntities;

Entity FindFloorEntity(ECS::Registry& reg) {
	auto entities = reg.GetEntitiesWith<NameComponent>();
	auto names = reg.GetRawArray<NameComponent>();

	for (size_t i = 0; i < entities.size(); ++i) {
		std::string nameLower(names[i].name.c_str());
		std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);

		// Scan for keywords commonly exported by Blender/glTF for floor meshes
		if (nameLower.contains("floor") || nameLower.contains("ground") ||
			nameLower.contains("lobby")) {
			// Ensure it actually carries visual geometry
			if (reg.Get<MeshComponent>(entities[i]) != nullptr) {
				return entities[i];
			}
		}
	}
	return NullEntity;
}

} // namespace

// ============================================================================
// GAME APPLICATION INTERFACE IMPLEMENTATION
// ============================================================================

bool ZHLN::InitializeGame(Engine& engine) {
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
	s_PlayerEntity = reg.Create();
	reg.Add(s_PlayerEntity, MovementComponent{});
	Entity charPhys = Physics::CreateCharacter(pc, JPH::RVec3(0.0f, 3.0f, 0.0f));
	reg.Add(s_PlayerEntity, PhysicsComponent{charPhys});

	// 4. Initialize Scripting Subsystem
	s_ScriptRunner = new ScriptRunner();
	s_ScriptRunner->RunFile("scripts/gameplay.lua");
	s_GameplayWatcher = new FileWatcher("scripts/gameplay.lua");
	s_FrameCounter = 0;

	// 5. Spawn Level Prefabs (Circus Lobby V9.glb)
	Scene scene{};
	scene.Setup(engine);

	// --- 5.1 QUERY THE SPAWNED ENVIRONMENT FOR THE FLOOR ENTITY ---
	s_VisualFloorEntity = FindFloorEntity(reg);
	if (s_VisualFloorEntity != NullEntity) {
		ZHLN::Log("[DOD Query] Successfully binded to glTF Floor Entity! Handle Index: {}",
				  s_VisualFloorEntity.index);
	} else {
		ZHLN::Log("[DOD Query] WARNING: Could not find floor mesh in glTF scene.");
	}

	AssetFactory::SetupPlayerRagdoll(rc, pc, reg, s_PlayerEntity, s_PomniParts);

	// Position Camera orientation initially looking forward
	cam.yaw = -90.0f;
	cam.pitch = -10.0f;

	s_FontAtlasIdx = AssetFactory::CreateFontAtlasTexture(rc);
	s_HelloText = GUI::CreateTextMesh(rc, "Zahlen Engine - TADC Dorm Showcase", 25.0f, 25.0f, 2.5f,
									  {0.9f, 0.1f, 0.1f, 1.0f});

	s_DebugLineMesh = AssetFactory::CreateBox(rc, {0.02f, 0.02f, 0.5f});
	s_DebugLineMaterial = AssetFactory::CreateBasicMaterial(rc);
	s_DebugLineMaterial.baseColorFactor[0] = 0.0f; // Neon Cyan
	s_DebugLineMaterial.baseColorFactor[1] = 1.0f;
	s_DebugLineMaterial.baseColorFactor[2] = 1.0f;
	s_DebugLineMaterial.baseColorFactor[3] = 1.0f;

	s_ArticulationSystem = new ArticulationSystem();
	s_AnimationSystem = new AnimationSystem();

	return true;
}

void ZHLN::UpdateGame(Engine& engine, float dt, float& physicsAccumulator) {
	auto& cam = engine.GetCamera();
	auto& pc = engine.GetPhysicsContext();
	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();

	// 1. Draw HUD Layouts
	ZHLN::DrawConsole(*s_ScriptRunner);
	ZHLN::DrawInventoryShell(*s_ScriptRunner);
	ZHLN::DrawProfiler(engine, s_TAAState);
	ZHLN::DrawOrientationGizmo(cam);

	// 2. Lighting Workspace Developer Menu
	ImGui::Begin("Lighting Workspace Controller");
	ImGui::Text("Specular Mips & Area Lights Debugger");
	ImGui::Separator();
	ImGui::SliderFloat("Sphere Light Radius", &s_SphereLightRadius, 0.0f, 5.0f);
	ImGui::SliderFloat("Cyan Intensity", &s_Light1Intensity, 0.0f, 500.0f);
	ImGui::SliderFloat("Magenta Intensity", &s_Light2Intensity, 0.0f, 500.0f);
	ImGui::Separator();
	ImGui::SliderFloat("Floor Roughness", &s_FloorRoughness, 0.0f, 1.0f);
	ImGui::SliderFloat("Floor Metallic", &s_FloorMetallic, 0.0f, 1.0f);

	// --- NEW: PARALLAX-CORRECTED PROBE CONTROLS ---
	ImGui::SeparatorText("Parallax-Corrected Local Reflection Probe");
	ImGui::Checkbox("Enable Box Projection", &s_UseLocalProbe);
	if (s_UseLocalProbe) {
		std::array<float, 3> minArr = {s_ProbeMin.GetX(), s_ProbeMin.GetY(), s_ProbeMin.GetZ()};
		std::array<float, 3> maxArr = {s_ProbeMax.GetX(), s_ProbeMax.GetY(), s_ProbeMax.GetZ()};
		std::array<float, 3> posArr = {s_ProbePos.GetX(), s_ProbePos.GetY(), s_ProbePos.GetZ()};

		if (ImGui::DragFloat3("Box Min", minArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
			s_ProbeMin = JPH::Vec3(minArr[0], minArr[1], minArr[2]);
		}
		if (ImGui::DragFloat3("Box Max", maxArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
			s_ProbeMax = JPH::Vec3(maxArr[0], maxArr[1], maxArr[2]);
		}
		if (ImGui::DragFloat3("Probe Position", posArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
			s_ProbePos = JPH::Vec3(posArr[0], posArr[1], posArr[2]);
		}
	}

	// --- NEW: COOPERATIVE SSAO / SSGI CONTROLLER ---
	ImGui::SeparatorText("Ambient Occlusion & Global Illumination");
	const char* giModesList[] = {"Off", "SSAO (Ambient Occlusion)", "SSGI (Screen Space GI)",
								 "HBAO (Horizon-Based AO)", "GTAO (Ground Truth AO)"};
	ImGui::Combo("GI Mode", &s_GIMode, giModesList, IM_ARRAYSIZE(giModesList));

	if (s_GIMode == 1) { // SSAO Settings
		ImGui::SliderFloat("AO Radius", &s_AORadius, 0.05f, 2.5f, "%.2fm");
		ImGui::SliderFloat("AO Bias", &s_AOBias, 0.001f, 0.2f, "%.3f");
		ImGui::SliderFloat("AO Contrast", &s_AOPower, 0.5f, 5.0f, "%.1fx");
		ImGui::SliderInt("AO Samples", &s_GISamples, 2, 32);
	} else if (s_GIMode == 2) { // SSGI Settings
		ImGui::SliderFloat("Bounce Radius", &s_AORadius, 0.05f, 2.5f, "%.2fm");
		ImGui::SliderFloat("Bounce Bias", &s_AOBias, 0.001f, 0.2f, "%.3f");
		ImGui::SliderFloat("GI Bounce Intensity", &s_GIIntensity, 0.1f, 5.0f, "%.1fx");
		ImGui::SliderInt("GI Samples", &s_GISamples, 2, 32);
	} else if (s_GIMode == 3 || s_GIMode == 4) { // HBAO / GTAO Settings
		ImGui::SliderFloat("Search Radius", &s_AORadius, 0.05f, 3.0f, "%.2fm");
		ImGui::SliderFloat("Acne Bias", &s_AOBias, 0.001f, 0.2f, "%.3f");
		ImGui::SliderFloat("Shadow Contrast", &s_AOPower, 0.5f, 6.0f, "%.1fx");
		ImGui::SliderInt("Search Steps", &s_GISamples, 4, 32); // Represents total step count
	}

	// --- NEW: VIGNETTE SLIDERS ---
	ImGui::Separator();
	ImGui::SeparatorText("Camera Vignette");
	ImGui::SliderFloat("Vignette Intensity", &s_VignetteIntensity, 0.0f, 2.5f, "%.2f");
	if (s_VignetteIntensity > 0.0f) {
		ImGui::SliderFloat("Vignette Power", &s_VignettePower, 0.1f, 6.0f, "%.2f");
	}

	// when you realize graphical settings were there as a byproduct of ImGUI debugging hell
	ImGui::Checkbox("Enable SSR", &s_EnableSSR);

	ImGui::End();

	// 3. Hot Reload Script Files
	if (++s_FrameCounter % 60 == 0 && s_GameplayWatcher->CheckModified()) {
		s_ScriptRunner->ReloadFile("scripts/gameplay.lua");
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

		s_ScriptRunner->CallUpdate(&engine, dt);

		// 4. Physics Tick loop
		physicsAccumulator += dt;
		constexpr float targetDt = 1.0f / 60.0f;
		while (physicsAccumulator >= targetDt) {
			ZHLN::MovementSystem(engine, targetDt);
			pc.Step(targetDt);
			physicsAccumulator -= targetDt;
		}

		// 5. Run animation systems
		s_AnimationSystem->UpdateAnimations(rc, reg, dt);
		s_ArticulationSystem->Update(engine, dt);
	}
}

void ZHLN::RenderGame(Engine& engine, float physicsAccumulator) {
	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();
	auto& pc = engine.GetPhysicsContext();

	// Static local clock to calculate independent frameTime delta for audio threading [1]
	static Clock deltaClock;
	float renderFrameTime = deltaClock.GetDeltaTime();

	auto res = engine.GetWindow().GetSize();
	const auto& worldState = pc.GetWorld();

	if (s_TAAState.enabled) {
		s_TAAState.frameIndex++;
	} else {
		s_TAAState.frameIndex = 0;
	}

	// 1. Retrieve & Interpolate Player Position from Jolt [6]
	JPH::Vec3 playerPos = JPH::Vec3::sZero();
	constexpr float targetDt = 1.0f / 60.0f;
	if (reg.IsAlive(s_PlayerEntity)) {
		if (auto* phys = reg.Get<PhysicsComponent>(s_PlayerEntity)) {
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
		s_CamDistance = JPH::Clamp(s_CamDistance - wheelDelta * 0.5f, 1.5f, 15.0f);
	}
	cam.position =
		playerPos - (offsetDir.Normalized() * s_CamDistance) + JPH::Vec3(0.0f, 1.3f, 0.0f);

	JPH::Mat44 unjitteredProj = cam.GetProjectionMatrix((float)res.width / res.height);
	JPH::Mat44 unjitteredVp = unjitteredProj * cam.GetViewMatrix();

	JPH::Mat44 vp{};
	if (s_TAAState.enabled) {
		vp = cam.GetJitteredProjectionMatrix((float)res.width / res.height, res.width, res.height,
											 s_TAAState) *
			 cam.GetViewMatrix();
	} else {
		vp = unjitteredVp;
	}

	static JPH::Mat44 s_FrozenVP = JPH::Mat44::sIdentity();
	static JPH::Vec3 s_FrustumCorners[8] = {};
	static bool s_WasFrozen = false;

	if (CullingStats::FreezeFrustum) {
		if (!s_WasFrozen) {
			s_FrozenVP = unjitteredVp;
			JPH::Mat44 invVP = unjitteredVp.Inversed();
			JPH::Vec4 ndc[8] = {{-1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, -1.0f, 0.0f, 1.0f},
								{1.0f, 1.0f, 0.0f, 1.0f},	{-1.0f, 1.0f, 0.0f, 1.0f},
								{-1.0f, -1.0f, 1.0f, 1.0f}, {1.0f, -1.0f, 1.0f, 1.0f},
								{1.0f, 1.0f, 1.0f, 1.0f},	{-1.0f, 1.0f, 1.0f, 1.0f}};

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

	CullingSystem<false>(engine, s_VisibleEntities, s_PomniParts);

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
	sceneLights[1].intensity = s_Light1Intensity;
	sceneLights[1].radius = s_SphereLightRadius;

	// --- LIGHT 2: MAGENTA SPHERE ---
	sceneLights[2].position[0] = 5.0f; // <-- FIX: Was sceneLights[1]
	sceneLights[2].position[1] = 4.0f;
	sceneLights[2].position[2] = 0.0f;
	sceneLights[2].type = 1;
	sceneLights[2].color[0] = 1.0f;
	sceneLights[2].color[1] = 0.0f;
	sceneLights[2].color[2] = 0.5f;
	sceneLights[2].intensity = s_Light2Intensity;
	sceneLights[2].radius = s_SphereLightRadius;

	// Sync modified material values directly to the queried glTF floor in the registry
	if (s_VisualFloorEntity != NullEntity) {
		if (auto* floorMeshComp = reg.Get<MeshComponent>(s_VisualFloorEntity)) {
			floorMeshComp->material.roughnessFactor = s_FloorRoughness;
			floorMeshComp->material.metallicFactor = s_FloorMetallic;
		}
	}

	FrameUniforms uniforms{};
	uniforms.viewProj = vp;
	uniforms.unjitteredViewProj = unjitteredVp;
	uniforms.prevUnjitteredViewProj = s_PrevUnjitteredVp;
	uniforms.lightSpaceMatrix = lightSpaceBiased;
	uniforms.invViewProj = unjitteredVp.Inversed();
	std::memcpy(&uniforms.camPos[0], &cam.position, sizeof(float) * 3);
	std::memcpy(&uniforms.lightDir[0], &sunDirection, sizeof(float) * 3);
	uniforms.lightCount = static_cast<uint32_t>(sceneLights.size());
	uniforms.probeMin = JPH::Vec4(s_ProbeMin, s_UseLocalProbe ? 1.0f : 0.0f);
	uniforms.probeMax = JPH::Vec4(s_ProbeMax, 0.0f);
	uniforms.probePos = JPH::Vec4(s_ProbePos, 0.0f);

	Renderer::SetGISettings(rc, {.mode = s_GIMode,
								 .aoRadius = s_AORadius,
								 .aoBias = s_AOBias,
								 .aoPower = s_AOPower,
								 .giIntensity = s_GIIntensity,
								 .giSamples = s_GISamples,
								 .vignetteIntensity = s_VignetteIntensity,
								 .vignettePower = s_VignettePower,
								 .enableSSR = s_EnableSSR ? 1 : 0});

	Renderer::SetLights(rc, sceneLights.data(), uniforms.lightCount); // Upload SSBO to GPU
	Renderer::SetFrameData(rc, uniforms, shadowProjView);

	Renderer::SetMatrices(rc, vp, unjitteredVp);

	JPH::Mat44 playerTransform = JPH::Mat44::sIdentity();
	if (reg.IsAlive(s_PlayerEntity)) {
		bool isRagdollActive = false;
		if (auto* ragComp = reg.Get<RagdollComponent>(s_PlayerEntity)) {
			isRagdollActive = (ragComp->state == RagdollState::Limp ||
							   ragComp->state == RagdollState::KeyframeMotor);
		}

		if (!isRagdollActive) {
			JPH::Quat rotation = JPH::Quat::sIdentity();
			if (auto* move = reg.Get<MovementComponent>(s_PlayerEntity)) {
				rotation = JPH::Quat(move->orientation[0], move->orientation[1],
									 move->orientation[2], move->orientation[3]);
			}
			playerTransform =
				Math::CreateTransform(playerPos - JPH::Vec3(0.0f, 0.8f, 0.0f), rotation);
		}
	}

	for (Entity e : s_VisibleEntities) {
		auto* mesh = reg.Get<MeshComponent>(e);
		if (mesh == nullptr) {
			continue;
		}

		bool isPlayerPart = std::ranges::contains(s_PomniParts, e);
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
	CullingStats::CulledObjects = CullingStats::TotalObjects - (uint32_t)s_VisibleEntities.size();

	Renderer::DrawUI(rc, s_HelloText, s_FontAtlasIdx);

	// --- DRAW THE FREEZE WIREFRAME FRUSTUM ---
	if (CullingStats::FreezeFrustum && s_DebugLineMesh.vertexBuffer != BufferHandle::Invalid) {
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

			Renderer::Draw(rc, s_DebugLineMaterial, s_DebugLineMesh, lineTransform, lineTransform,
						   len);
		}
	}

	rc.SetTAAState(s_TAAState);

	engine.EndFrame();

	ZHLN::AudioSystem(engine, renderFrameTime); // Decoupled frametime for precise audio pacing [1]

	s_PrevUnjitteredVp = unjitteredVp;

	// Global update for prevTransforms
	auto allEntities = reg.GetEntitiesWith<MeshComponent>();
	for (Entity e : allEntities) {
		auto* mesh = reg.Get<MeshComponent>(e);
		if (mesh != nullptr) {
			JPH::Mat44 currentTransform{};
			bool isPlayerPart = std::ranges::contains(s_PomniParts, e);

			if (isPlayerPart) {
				currentTransform = playerTransform * mesh->localTransform;
			} else {
				currentTransform = mesh->localTransform;
			}
			mesh->prevTransform = currentTransform;
		}
	}
}

void ZHLN::ShutdownGame([[maybe_unused]] Engine& engine) {
	// Reclaim heap memories
	delete s_ScriptRunner;
	delete s_GameplayWatcher;
	delete s_ArticulationSystem;
	delete s_AnimationSystem;
}

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
				   .enableValidation = false},
	};

	{
		Engine engine(config);
		engine.GetWindow().Focus();

		if (!ZHLN::InitializeGame(engine)) {
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

			engine.BeginFrame();

			// Execute game simulation ticks and rendering
			ZHLN::UpdateGame(engine, frameTime, physicsAccumulator);
			ZHLN::RenderGame(engine, physicsAccumulator);
		}

		ZHLN::ShutdownGame(engine);
	}

	TaskSystem::Shutdown();
	return EXIT_SUCCESS;
}
