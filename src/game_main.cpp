// src/game_main.cpp
#include "Zahlen/Input.hpp"
#include "Zahlen/alife/Types.hpp"
#include "ecs/ECS.hpp"
#include "engine/FileWatcher.hpp"
#include "engine/Platform.hpp"
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
#include <engine/system/ArticulationSystem.hpp>
#include <physics/PhysicsWorld.hpp>
#include <string>
#include <threading/Mutex.hpp>
#include <threading/TaskSystem.hpp>
#include <vector>
namespace ZHLN {
void DrawConsole(ScriptRunner& runner);
void DrawProfiler(Engine& engine);
void MovementSystem(Engine& engine, float dt);
void AudioSystem(Engine& engine, float dt);
void LoadLevel(Engine& engine, const std::string& path, Material material);
void DrawOrientationGizmo(const ZHLN::Camera& cam);

// Define the 12 edges connecting the 8 frustum corners
struct FrustumEdge {
	int start;
	int end;
};
} // namespace ZHLN

using namespace ZHLN;

namespace {

// We track which GLB entities belong to Pomni so we can translate them relative to the physics
// capsule
static std::vector<Entity> s_PomniParts;
static Entity s_PlayerEntity = NullEntity;
static float s_CamDistance = 4.5f;

static constexpr FrustumEdge s_FrustumEdges[12] = {
	{.start = 0, .end = 1}, {.start = 1, .end = 2},
	{.start = 2, .end = 3}, {.start = 3, .end = 0}, // Near Plane loop
	{.start = 4, .end = 5}, {.start = 5, .end = 6},
	{.start = 6, .end = 7}, {.start = 7, .end = 4}, // Far Plane loop
	{.start = 0, .end = 4}, {.start = 1, .end = 5},
	{.start = 2, .end = 6}, {.start = 3, .end = 7} // Near-to-Far connection lines
};

// Pre-allocated debug wireframe resources to avoid mid-frame Vulkan allocations [2]
static Mesh s_DebugLineMesh = {};
static Material s_DebugLineMaterial = {};

struct Scene {
	void Setup(Engine& engine) {
		auto& rc = engine.GetRenderContext();
		auto& reg = engine.GetRegistry();

		ZHLN::Log("Assembling Scene with Pure Runtime glTF Parsing...");

		// 1. Spawns room with physics colliders, but flags as STATIC (No animation overhead!)
		AssetFactory::SpawnGLB<true, false>(rc, reg, "Circus Lobby V9.glb", nullptr, 0);

		// 2. Spawns Pomni as ANIMATED, with no static physics colliders (handles collision via
		// Character Controller)
		s_PomniParts.resize(128);
		uint32_t pomniCount = AssetFactory::SpawnGLB<false, true>(
			rc, reg, "tadc_models/POMNI.glb", s_PomniParts.data(), (uint32_t)s_PomniParts.size());
		s_PomniParts.resize(pomniCount);
	}
};

JPH::Array<ZHLN::Entity> s_VisibleEntities;
JPH::Vec3 s_LastCullPos;
float s_LastCullYaw = 0.0f;

void UpdateCulling(Engine& engine) {
	ZHLN_PROFILE_SCOPE("Culling (ECS O(N))");
	auto& cam = engine.GetCamera();
	auto& reg = engine.GetRegistry();
	auto& pc = engine.GetPhysicsContext();
	const auto& worldState = pc.GetWorld();

	auto entities = reg.GetEntitiesWith<MeshComponent>();

	if (!CullingStats::EnableCulling) {
		s_VisibleEntities.assign(entities.begin(), entities.end());
		return;
	}

	s_VisibleEntities.clear();
	auto meshes = reg.GetRawArray<MeshComponent>();

	// 1. Retrieve the player's physical coordinates for culling calculations [6]
	JPH::Vec3 playerPos = JPH::Vec3::sZero();
	if (reg.IsAlive(s_PlayerEntity)) {
		if (auto* phys = reg.Get<PhysicsComponent>(s_PlayerEntity)) {
			uint32_t dense = worldState.slotToDense[phys->physicsHandle.index];
			const size_t base = static_cast<size_t>(dense) * 4;
			playerPos =
				JPH::Vec3((float)worldState.positions[base], (float)worldState.positions[base + 1],
						  (float)worldState.positions[base + 2]);
		}
	}

	for (size_t i = 0; i < entities.size(); ++i) {
		Entity e = entities[i];
		JPH::Vec3 pos = meshes[i].localTransform.GetTranslation();

		// 2. If this mesh is part of Pomni, offset the culling sphere by her physical coordinate
		// [6]
		bool isPlayerPart =
			std::find(s_PomniParts.begin(), s_PomniParts.end(), e) != s_PomniParts.end();
		if (isPlayerPart) {
			pos = playerPos + pos;
		}

		if (cam.frustum.IsSphereVisible(pos, meshes[i].cullRadius)) {
			s_VisibleEntities.push_back(e);
		}
	}

	s_LastCullPos = cam.position;
	s_LastCullYaw = cam.yaw;
}
} // namespace

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
	Platform::Init();
	ZHLN::SetupSignalHandler();
	TaskSystem::Init();
	Clock clock;

	ZHLN::EngineConfig config{
		.physics = {.maxBodies = 1000, .maxBodyPairs = 2000, .maxContactConstraints = 2000},
		.render = {.appName = "Zahlen Engine - Digital Circus Showcase",
				   .width = 1280,
				   .height = 720,
				   .vsync = false,
				   .enableValidation = true},
	};

	Engine engine(config);
	engine.GetWindow().Focus();

	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();
	auto& pc = engine.GetPhysicsContext();

	ZHLN::ArticulationSystem articulationSystem;

	// ------------------------------------------------------------------------
	// Register native components so the Lua FFI can resolve them!
	// ------------------------------------------------------------------------
	reg.RegisterComponent<MeshComponent>("MeshComponent");
	reg.RegisterComponent<PhysicsComponent>("PhysicsComponent");
	reg.RegisterComponent<MovementComponent>("MovementComponent");
	reg.RegisterComponent<ALife::ALifeComponent>("ALifeComponent");

	// ------------------------------------------------------------------------
	// 1. Create a Solid Physics Ground Plane to stand on
	// ------------------------------------------------------------------------
	auto groundShape =
		Physics::GetOrCreateShape(pc, Physics::ShapeType::Plane, 0.0f, 1.0f, 0.0f, 0.0f);
	Entity ground = reg.Create();
	reg.Add(ground,
			PhysicsComponent{Physics::CreateRigidBody(
				pc, groundShape, {0, 0, 0}, JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0)});

	// ------------------------------------------------------------------------
	// 2. Create the Player Entity with Character Controller
	// ------------------------------------------------------------------------
	Entity player = reg.Create();
	reg.Add(player, MovementComponent{});
	Entity charPhys = Physics::CreateCharacter(pc, JPH::RVec3(0.0f, 3.0f, 0.0f));
	reg.Add(player, PhysicsComponent{charPhys});
	s_PlayerEntity = player;

	ScriptRunner scriptRunner;
	scriptRunner.RunFile("scripts/gameplay.lua");
	FileWatcher gameplayWatcher("scripts/gameplay.lua");
	uint32_t frameCounter = 0;

	Scene scene{};
	scene.Setup(engine);

	// Position Camera orientation initially looking forward
	cam.yaw = -90.0f;
	cam.pitch = -10.0f;

	uint32_t fontAtlasIdx = AssetFactory::CreateFontAtlasTexture(rc);

	Mesh helloText = GUI::CreateTextMesh(rc, "Zahlen Engine - TADC Dorm Showcase", 25.0f, 25.0f,
										 2.5f, {0.9f, 0.1f, 0.1f, 1.0f});

	// --- PRE-INITIALIZE DEBUG FRUSTUM WIREFRAME RESOURCES ---
	s_DebugLineMesh = AssetFactory::CreateBox(rc, {0.02f, 0.02f, 0.5f});
	s_DebugLineMaterial = AssetFactory::CreateBasicMaterial(rc);
	s_DebugLineMaterial.baseColorFactor[0] = 0.0f; // Neon Cyan
	s_DebugLineMaterial.baseColorFactor[1] = 1.0f;
	s_DebugLineMaterial.baseColorFactor[2] = 1.0f;
	s_DebugLineMaterial.baseColorFactor[3] = 1.0f;

	float physicsAccumulator = 0.0f;
	const float targetDt = 1.0f / 60.0f;

	while (engine.IsRunning()) {
		float frameTime = clock.GetDeltaTime();

		engine.ProcessEvents();

		if (engine.GetInput().IsKeyDown(KeyCode::Escape)) {
			engine.GetWindow().Close();
		}

		ZHLN::DrawConsole(scriptRunner);
		ZHLN::DrawProfiler(engine);
		ZHLN::DrawOrientationGizmo(cam);

		if (engine.GetInput().NeedsResize()) {
			rc.SetResolution(engine.GetInput().GetNewSize());
			engine.GetInput().ClearResizeFlag();
			continue;
		}

		if (++frameCounter % 60 == 0 && gameplayWatcher.CheckModified()) {
			scriptRunner.ReloadFile("scripts/gameplay.lua");
		}

		{
			ZHLN_PROFILE_SCOPE("Logic");

			// Mouse look (Active only when holding Right Click)
			const float sensitivity = 0.15f;
			if (engine.GetInput().IsMouseButtonDown(KeyCode::RButton)) {
				cam.yaw += engine.GetInput().GetMouse().deltaX * sensitivity;
				cam.pitch = std::clamp(
					cam.pitch - (engine.GetInput().GetMouse().deltaY * sensitivity), -89.0f, 89.0f);
			}

			scriptRunner.CallUpdate(&engine, frameTime);

			// ----------------------------------------------------------------
			// 3. Physics Simulation Accumulator
			// ----------------------------------------------------------------
			physicsAccumulator += frameTime;
			while (physicsAccumulator >= targetDt) {
				// Solve movement vector using inputs populated from Lua scripts
				ZHLN::MovementSystem(engine, targetDt);

				// Advance Jolt simulation
				pc.Step(targetDt);

				physicsAccumulator -= targetDt;
			}

			// --- STEP 4C: Play embedded skeletal keyframes over time ---
			AssetFactory::UpdateAnimations(rc, reg, frameTime);
			articulationSystem.Update(engine, frameTime);
		}

		auto res = engine.GetWindow().GetSize();

		if (res.width > 0 && res.height > 0) {
			const auto& worldState = pc.GetWorld();

			if (g_TAAState.enabled) {
				g_TAAState.frameIndex++;
			} else {
				g_TAAState.frameIndex = 0;
			}

			// ----------------------------------------------------------------
			// 4. Retrieve & Interpolate Player Position from Jolt [6]
			// ----------------------------------------------------------------
			JPH::Vec3 playerPos = JPH::Vec3::sZero();
			if (reg.IsAlive(s_PlayerEntity)) {
				if (auto* phys = reg.Get<PhysicsComponent>(s_PlayerEntity)) {
					uint32_t dense = worldState.slotToDense[phys->physicsHandle.index];
					const size_t base = static_cast<size_t>(dense) * 4;

					// Current frame position from Jolt [6]
					JPH::Vec3 currPos((float)worldState.positions[base],
									  (float)worldState.positions[base + 1],
									  (float)worldState.positions[base + 2]);

					// Previous frame position from Jolt [6]
					JPH::Vec3 prevPos((float)worldState.prevPositions[base],
									  (float)worldState.prevPositions[base + 1],
									  (float)worldState.prevPositions[base + 2]);

					// Calculate sub-frame remainder ratio (Alpha) [6]
					float alpha = std::clamp(physicsAccumulator / targetDt, 0.0f, 1.0f);

					// Smoothly interpolate to get exact sub-frame render coordinate [6]
					playerPos = prevPos + alpha * (currPos - prevPos);
				}
			}
			// ----------------------------------------------------------------
			// 5. Orbit Camera Positioning (Follow Follow)
			// ----------------------------------------------------------------
			float yawRad = JPH::DegreesToRadians(cam.yaw);
			float pitchRad = JPH::DegreesToRadians(cam.pitch);
			JPH::Vec3 offsetDir(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad),
								JPH::Sin(yawRad) * JPH::Cos(pitchRad));

			float wheelDelta = engine.GetInput().GetMouse().wheel;
			if (std::abs(wheelDelta) > 0.01f) {
				// Scroll up zoom-in, scroll down zoom-out
				s_CamDistance = JPH::Clamp(s_CamDistance - wheelDelta * 0.5f, 1.5f, 15.0f);
			}
			// Place the camera 4.5 meters behind the player capsule
			cam.position =
				playerPos - (offsetDir.Normalized() * s_CamDistance) + JPH::Vec3(0.0f, 1.3f, 0.0f);

			JPH::Mat44 unjitteredProj = cam.GetProjectionMatrix((float)res.width / res.height);
			JPH::Mat44 unjitteredVp = unjitteredProj * cam.GetViewMatrix();

			JPH::Mat44 vp{};
			if (g_TAAState.enabled) {
				vp = cam.GetJitteredProjectionMatrix((float)res.width / res.height, res.width,
													 res.height) *
					 cam.GetViewMatrix();
			} else {
				vp = unjitteredVp;
			}

			static JPH::Mat44 s_FrozenVP = JPH::Mat44::sIdentity();
			static JPH::Vec3 s_FrustumCorners[8] = {};
			static bool s_WasFrozen = false;

			if (CullingStats::FreezeFrustum) {
				if (!s_WasFrozen) {
					// Freeze the current View-Projection matrix [1]
					s_FrozenVP = unjitteredVp;

					// Calculate the 8 corners of the static view frustum in world space [1]
					JPH::Mat44 invVP = unjitteredVp.Inversed();
					JPH::Vec4 ndc[8] = {
						// Near Plane (Z = 0.0 in Vulkan depth)
						{-1.0f, -1.0f, 0.0f, 1.0f}, // Top-Left
						{1.0f, -1.0f, 0.0f, 1.0f},	// Top-Right
						{1.0f, 1.0f, 0.0f, 1.0f},	// Bottom-Right
						{-1.0f, 1.0f, 0.0f, 1.0f},	// Bottom-Left
						// Far Plane (Z = 1.0 in Vulkan depth)
						{-1.0f, -1.0f, 1.0f, 1.0f}, // Top-Left
						{1.0f, -1.0f, 1.0f, 1.0f},	// Top-Right
						{1.0f, 1.0f, 1.0f, 1.0f},	// Bottom-Right
						{-1.0f, 1.0f, 1.0f, 1.0f}	// Bottom-Left
					};

					for (int i = 0; i < 8; ++i) {
						JPH::Vec4 worldPos = invVP * ndc[i];
						float w = worldPos.GetW();
						if (std::abs(w) > 1e-6f) {
							s_FrustumCorners[i] = JPH::Vec3(
								worldPos.GetX() / w, worldPos.GetY() / w, worldPos.GetZ() / w);
						}
					}
					s_WasFrozen = true;
				}
				// Lock frustum calculations to the captured matrix [1]
				cam.frustum.Update(s_FrozenVP);
			} else {
				// Normal culling path [1]
				cam.frustum.Update(unjitteredVp);
				s_WasFrozen = false;
			}

			UpdateCulling(engine);

			JPH::Vec3 sunDirection = {-0.6f, 0.4f, -0.7f};
			JPH::Mat44 lightView =
				Math::CreateLookAt(sunDirection * 100.0f, {0.0f, 0.0f, 0.0f}, JPH::Vec3::sAxisY());
			JPH::Mat44 lightProj = Math::CreateOrtho(-50.0f, 50.0f, -50.0f, 50.0f, 0.1f, 200.0f);
			JPH::Mat44 shadowProjView = lightProj * lightView;

			// Bias Matrix mapping depth safely
			JPH::Mat44 biasMatrix = {
				JPH::Vec4(0.5f, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, -0.5f, 0.0f, 0.0f),
				JPH::Vec4(0.0f, 0.0f, 1.0f, 0.0f), JPH::Vec4(0.5f, 0.5f, 0.0f, 1.0f)};

			static JPH::Mat44 s_PrevUnjitteredVp = unjitteredVp;
			static bool s_FirstFrame = true;
			if (s_FirstFrame) {
				s_PrevUnjitteredVp = unjitteredVp;
				s_FirstFrame = false;
			}

			FrameUniforms uniforms{};
			uniforms.viewProj = vp;
			uniforms.unjitteredViewProj = unjitteredVp;
			uniforms.prevUnjitteredViewProj = s_PrevUnjitteredVp;
			std::memcpy(&uniforms.camPos[0], &cam.position, sizeof(float) * 3);
			std::memcpy(&uniforms.lightDir[0], &sunDirection, sizeof(float) * 3);
			uniforms.lightCount = 0;

			Renderer::SetFrameData(rc, uniforms, shadowProjView);

			engine.BeginFrame();
			Renderer::SetMatrices(rc, vp, unjitteredVp);

			static JPH::Quat s_PlayerRotation = JPH::Quat::sIdentity();

			JPH::Vec3 velocity = Physics::GetCharacterVelocity(pc, charPhys);
			JPH::Vec3 flatVel(velocity.GetX(), 0.0f, velocity.GetZ());

			if (flatVel.LengthSq() > 0.1f) {
				// Calculate target orientation based on velocity
				float targetAngleRad =
					std::atan2(-velocity.GetZ(), velocity.GetX()) + JPH::DegreesToRadians(90.0f);
				JPH::Quat targetRotation =
					JPH::Quat::sRotation(JPH::Vec3::sAxisY(), targetAngleRad);

				// Smoothly interpolate (SLERP) towards the target rotation [6]
				// Turn rate of 10.0f means Pomni will complete turns over ~100ms
				float turnSpeed = 10.0f;
				s_PlayerRotation = s_PlayerRotation.SLERP(
					targetRotation, JPH::Clamp(turnSpeed * frameTime, 0.0f, 1.0f));
			}

			JPH::Mat44 playerTransform = JPH::Mat44::sIdentity();
			if (reg.IsAlive(s_PlayerEntity)) {
				playerTransform = Math::CreateTransform(
					playerPos - JPH::Vec3(0.0f, 0.5f, 0.0f), // Match physical capsule bottom
					s_PlayerRotation);
			}
			for (Entity e : s_VisibleEntities) {
				auto* mesh = reg.Get<MeshComponent>(e);
				if (mesh == nullptr) {
					continue;
				}

				// Check if this submesh entity is part of the player character hierarchy
				bool isPlayerPart =
					std::find(s_PomniParts.begin(), s_PomniParts.end(), e) != s_PomniParts.end();
				JPH::Mat44 currentTransform{};
				if (isPlayerPart) {
					currentTransform = playerTransform * mesh->localTransform;
				} else {
					currentTransform = mesh->localTransform;
				}

				Renderer::Draw(rc, mesh->material, mesh->mesh, currentTransform,
							   mesh->prevTransform, mesh->cullRadius, mesh->jointOffset,
							   mesh->isSkinned, mesh->morphOffset, mesh->activeMorphCount,
							   mesh->morphWeights);
			}

			CullingStats::TotalObjects = (uint32_t)reg.GetEntitiesWith<MeshComponent>().size();
			CullingStats::CulledObjects =
				CullingStats::TotalObjects - (uint32_t)s_VisibleEntities.size();

			Renderer::DrawUI(rc, helloText, fontAtlasIdx);

			// --- DRAW THE FREEZE WIREFRAME FRUSTUM ---
			if (CullingStats::FreezeFrustum &&
				s_DebugLineMesh.vertexBuffer != BufferHandle::Invalid) {
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
					JPH::Mat44 lineTransform =
						Math::CreateTransform(mid, rot, JPH::Vec3(1.0f, 1.0f, len));

					Renderer::Draw(rc, s_DebugLineMaterial, s_DebugLineMesh, lineTransform,
								   lineTransform, len);
				}
			}

			engine.EndFrame();

			// Run audio system updates
			ZHLN::AudioSystem(engine, frameTime);

			s_PrevUnjitteredVp = unjitteredVp;

			// Global update for prevTransforms to prevent TAA motion vector spasms
			auto allEntities = reg.GetEntitiesWith<MeshComponent>();
			for (Entity e : allEntities) {
				auto* mesh = reg.Get<MeshComponent>(e);
				if (mesh != nullptr) {
					JPH::Mat44 currentTransform{};
					bool isPlayerPart = std::find(s_PomniParts.begin(), s_PomniParts.end(), e) !=
										s_PomniParts.end();

					if (isPlayerPart) {
						currentTransform = playerTransform * mesh->localTransform;
					} else {
						currentTransform = mesh->localTransform;
					}
					mesh->prevTransform =
						currentTransform; // Must match the evaluated render transform!
				}
			}
		} else {
			Platform::Sleep(10);
		}
	}

	ZHLN::Log("Shutting down engine...");
	return 0;
}
