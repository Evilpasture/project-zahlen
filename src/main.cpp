#include "engine/FileWatcher.hpp"
#include "engine/Platform.hpp"
#include "imgui.h"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Scripting.hpp>
#include <algorithm>
#include <detail/ControlFlow.hpp>
#include <physics/PhysicsWorld.hpp>
#include <threading/Mutex.hpp>
#include <threading/TaskSystem.hpp>

namespace ZHLN {
void DrawConsole(ScriptRunner& runner);
void DrawProfiler(Engine& engine);
void MovementSystem(Engine& engine, float dt);
} // namespace ZHLN

using namespace ZHLN;

struct Scene {
	ZHLN::Entity playerEntity;

	void Setup(Engine& engine) {
		auto& rc = engine.GetRenderContext();
		auto& pc = engine.GetPhysicsContext();
		auto& reg = engine.GetRegistry();

		reg.RegisterComponent<MeshComponent>();
		reg.RegisterComponent<PhysicsComponent>();
		reg.RegisterComponent<MovementComponent>();

		// --- Assets ---
		Mesh floorMesh = AssetFactory::CreatePlane(rc, 100.0f, {0.1f, 0.1f, 0.12f, 1.0f});
		Mesh boxMesh = AssetFactory::CreateBox(rc, {0.5f, 0.5f, 0.5f}, {0.8f, 0.3f, 0.2f, 1.0f});
		Mesh playerMesh = AssetFactory::CreateBox(rc, {0.5f, 0.9f, 0.5f}, {0.2f, 0.6f, 0.9f, 1.0f});
		Material material = AssetFactory::CreateBasicMaterial(rc);

		auto floorShape =
			Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 100.0f, 1.0f, 100.0f);
		auto boxShape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 0.5f, 0.5f, 0.5f);

		// --- Spawn ---
		reg.Add(reg.Create(), MeshComponent{floorMesh, material, 150.0f},
				PhysicsComponent{Physics::CreateRigidBody(pc, floorShape, {0.0f, -1.0f, 0.0f},
														  JPH::Quat::sIdentity(),
														  JPH::EMotionType::Static, 0)});

		for (int i = 0; i < 2000; ++i) {
			float x = (float)(i % 45) - 22.5f;
			float z = (float)(i / 45.0f) - 22.5f;
			Entity propPhys =
				Physics::CreateRigidBody(pc, boxShape, {x, 5.0f + (i * 0.1f), z},
										 JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, 1);

			reg.Add(reg.Create(), MeshComponent{boxMesh, material, 1.0f},
					PhysicsComponent{propPhys});
		}

		playerEntity = reg.Create();
		reg.Add(playerEntity, MeshComponent{playerMesh, material, 1.5f});
		reg.Add(playerEntity, PhysicsComponent{Physics::CreateCharacter(pc, {0.0f, 2.0f, 0.0f})});
		reg.Add(playerEntity, MovementComponent{.speed = 8.0f});

		// Ensure the spatial tree is ready for the first frame
		pc.OptimizeBroadphase();
	}
};

// --- Global/Static state for temporal stability ---
static JPH::Array<ZHLN::Entity> s_VisibleEntities;
static JPH::Vec3 s_LastCullPos;
static float s_LastCullYaw = 0.0f;

void UpdateCameraSystem(Camera& cam, InputContext& input, Entity player, ECS::Registry& reg,
						const Physics::PhysicsWorld& world) {
	if (input.IsMouseButtonDown(KeyCode::RButton)) {
		cam.yaw += input.GetMouse().deltaX * 0.2f;
		cam.pitch = std::clamp(cam.pitch - input.GetMouse().deltaY * 0.2f, -89.0f, 89.0f);
	}

	ZHLN_LOCK(world.sync.shadowLock) {
		if (auto* pComp = reg.Get<PhysicsComponent>(player)) {
			uint32_t dense = world.slotToDense[pComp->physicsHandle.index];
			JPH::Real* p = &world.positions[dense * 4];
			JPH::Vec3 target = {(float)p[0], (float)p[1] + 1.0f, (float)p[2]};

			float yR = JPH::DegreesToRadians(cam.yaw), pR = JPH::DegreesToRadians(cam.pitch);
			JPH::Vec3 dir(JPH::Cos(yR) * JPH::Cos(pR), JPH::Sin(pR), JPH::Sin(yR) * JPH::Cos(pR));
			cam.position = target - (dir.Normalized() * 10.0f);
		}
	}
}

void UpdateCulling(Engine& engine) {
	ZHLN_PROFILE_SCOPE("Culling (ECS O(N))");
	auto& cam = engine.GetCamera();
	auto& reg = engine.GetRegistry();
	const auto& world = engine.GetPhysicsContext().GetWorld();

	auto entities = reg.GetEntitiesWith<MeshComponent>();

	// If culling is disabled, just render everything
	if (!CullingStats::EnableCulling) {
		s_VisibleEntities.assign(entities.begin(), entities.end());
		return;
	}

	bool moved = (cam.position - s_LastCullPos).LengthSq() > 0.01f ||
				 std::abs(cam.yaw - s_LastCullYaw) > 0.5f;
	if (!moved && !s_VisibleEntities.empty())
		return;

	s_VisibleEntities.clear();
	auto meshes = reg.GetRawArray<MeshComponent>();

	ZHLN_LOCK(world.sync.shadowLock) {
		for (size_t i = 0; i < entities.size(); ++i) {
			Entity e = entities[i];
			auto* phys = reg.Get<PhysicsComponent>(e);
			if (!phys)
				continue;

			uint32_t dense = world.slotToDense[phys->physicsHandle.index];
			JPH::Vec3 pos((float)world.positions[dense * 4], (float)world.positions[dense * 4 + 1],
						  (float)world.positions[dense * 4 + 2]);

			if (cam.frustum.IsSphereVisible(pos, meshes[i].cullRadius)) {
				s_VisibleEntities.push_back(e);
			}
		}
	}

	s_LastCullPos = cam.position;
	s_LastCullYaw = cam.yaw;
}

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
	Platform::Init();
	ZHLN::SetupSignalHandler();
	TaskSystem::Init();
	Clock clock;

	ZHLN::EngineConfig config{
		.physics = {.maxBodies = 10000, .maxBodyPairs = 20000, .maxContactConstraints = 20000},
		.render = {.width = 1280, .height = 720, .vsync = false, .enableValidation = true},
	};

	Engine engine(config);
	engine.GetWindow().Focus();

	auto& rc = engine.GetRenderContext();
	auto& pc = engine.GetPhysicsContext();
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();

	ScriptRunner scriptRunner;
	scriptRunner.RunFile("scripts/gameplay.lua");
	FileWatcher gameplayWatcher("scripts/gameplay.lua");
	uint32_t frameCounter = 0;

	Scene scene;
	scene.Setup(engine);

	float accumulator = 0.0f;
	const float targetDt = 1.0f / 60.0f;

	while (engine.IsRunning()) {
		float frameTime = clock.GetDeltaTime();
		accumulator += frameTime;

		engine.ProcessEvents();

		if (engine.GetInput().IsKeyDown(KeyCode::Escape)) {
			engine.GetWindow().Close();
		}

		ZHLN::DrawConsole(scriptRunner);
		ZHLN::DrawProfiler(engine);

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
			if (!ImGui::GetIO().WantCaptureKeyboard) {
				scriptRunner.CallUpdate(&engine, frameTime);
			}
			MovementSystem(engine, frameTime);
		}

		while (accumulator >= targetDt) {
			pc.Step(targetDt);
			accumulator -= targetDt;
		}

		// --- Core Orchestration ---
		const auto& world = pc.GetWorld();
		auto res = engine.GetWindow().GetSize();

		if (res.width > 0 && res.height > 0) {
			if (g_TAAState.enabled) {
				g_TAAState.frameIndex++;
			} else {
				g_TAAState.frameIndex = 0; // Freeze jitter when disabled
			}

			UpdateCameraSystem(cam, engine.GetInput(), scene.playerEntity, reg, world);

			JPH::Mat44 unjitteredProj = cam.GetProjectionMatrix((float)res.width / res.height);
			JPH::Mat44 unjitteredVp = unjitteredProj * cam.GetViewMatrix();

			// Check toggle before jittering
			JPH::Mat44 vp;
			if (g_TAAState.enabled) {
				vp = cam.GetJitteredProjectionMatrix((float)res.width / res.height, res.width,
													 res.height) *
					 cam.GetViewMatrix();
			} else {
				vp = unjitteredVp;
			}

			cam.frustum.Update(vp);
			UpdateCulling(engine);

			engine.BeginFrame();
			Renderer::Clear(rc, {0.08f, 0.09f, 0.12f, 1.0f});

			// Pass both matrices to the RenderContext
			Renderer::SetMatrices(rc, vp, unjitteredVp);

			ZHLN_LOCK(world.sync.shadowLock) {
				// Update DrawVisibleScene inline here (or pass reg/world to it)
				for (Entity e : s_VisibleEntities) {
					auto* mesh = reg.Get<MeshComponent>(e);
					auto* phys = reg.Get<PhysicsComponent>(e);
					if (!mesh || !phys)
						continue;

					uint32_t dense = world.slotToDense[phys->physicsHandle.index];
					JPH::Vec3 pos((float)world.positions[dense * 4],
								  (float)world.positions[dense * 4 + 1],
								  (float)world.positions[dense * 4 + 2]);
					JPH::Quat rot(world.rotations[dense * 4], world.rotations[dense * 4 + 1],
								  world.rotations[dense * 4 + 2], world.rotations[dense * 4 + 3]);

					JPH::Mat44 currentTransform = Math::CreateTransform(pos, rot);

					// Pass BOTH current and prev transforms
					Renderer::Draw(rc, mesh->material, mesh->mesh, currentTransform,
								   mesh->prevTransform);

					// Save this frame's transform for next frame
					mesh->prevTransform = currentTransform;
				}
			}

			CullingStats::TotalObjects = (uint32_t)reg.GetEntitiesWith<MeshComponent>().size();
			CullingStats::CulledObjects =
				CullingStats::TotalObjects - (uint32_t)s_VisibleEntities.size();

			engine.EndFrame();
		} else {
			Platform::Sleep(10);
		}
	}

	ZHLN::Log("Shutting down engine...");
	return 0;
}
