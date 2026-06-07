// src/game_main.cpp
#include "Zahlen/Input.hpp"
#include "Zahlen/alife/Types.hpp"
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
} // namespace ZHLN

using namespace ZHLN;

namespace {

// We track which GLB entities belong to Pomni so we can translate them relative to the physics
// capsule
static std::vector<Entity> s_PomniParts;
static Entity s_PlayerEntity = NullEntity;
static float s_CamDistance = 4.5f;

struct Scene {
	void Setup(Engine& engine) {
		auto& rc = engine.GetRenderContext();
		auto& reg = engine.GetRegistry();

		ZHLN::Log("Assembling Scene with Pure Runtime glTF Parsing...");

		// 1. Spawns the entire Room, parses all PBR materials, AND builds Jolt colliders
		// automatically!
		AssetFactory::SpawnGLB<true>(rc, reg, "Circus Lobby V9.glb");

		// 2. Spawns Pomni skinned meshes
		s_PomniParts = AssetFactory::SpawnGLB(rc, reg, "tadc_models/POMNI.glb");
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
				   .enableValidation = false},
	};

	Engine engine(config);
	engine.GetWindow().Focus();

	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();
	auto& pc = engine.GetPhysicsContext();

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

			cam.frustum.Update(unjitteredVp);
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

			static float s_PlayerYaw = -90.0f;

			JPH::Vec3 velocity = Physics::GetCharacterVelocity(pc, charPhys);
			JPH::Vec3 flatVel(velocity.GetX(), 0.0f, velocity.GetZ());

			if (flatVel.LengthSq() > 0.1f) {
				float movementAngleDeg =
					JPH::RadiansToDegrees(std::atan2(-velocity.GetZ(), velocity.GetX()));
				s_PlayerYaw = movementAngleDeg + 90.0f;
			}

			JPH::Mat44 playerTransform = JPH::Mat44::sIdentity();
			if (reg.IsAlive(s_PlayerEntity)) {
				playerTransform = Math::CreateTransform(
					playerPos - JPH::Vec3(0.0f, 0.0f, 0.0f), // Match physical capsule bottom
					JPH::Quat::sRotation(JPH::Vec3::sAxisY(), JPH::DegreesToRadians(s_PlayerYaw)));
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
