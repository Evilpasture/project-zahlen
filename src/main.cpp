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

			// 2. Give the small boxes a tight radius
			reg.Add(reg.Create(), MeshComponent{boxMesh, material, 1.0f},
					PhysicsComponent{propPhys});
		}

		playerEntity = reg.Create();
		reg.Add(playerEntity, MeshComponent{playerMesh, material, 1.5f});
		reg.Add(playerEntity, PhysicsComponent{Physics::CreateCharacter(pc, {0.0f, 2.0f, 0.0f})});
		reg.Add(playerEntity, MovementComponent{.speed = 8.0f});
	}
};

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
	Platform::Init();
	ZHLN::SetupSignalHandler();
	TaskSystem::Init();
	Clock clock;
	// 1. Setup the specific needs for THIS game
	ZHLN::EngineConfig config{
		.physics =
			{
				.maxBodies = 10000,
				.maxBodyPairs = 20000,
				.maxContactConstraints = 20000,
				.tempAllocatorSize = 64 * 1024 * 1024,
			},
		.render =
			{
				.width = 1280,
				.height = 720,
				.vsync = false,
			},
	};

	Engine engine(config);
	engine.GetWindow().Focus();

	auto& rc = engine.GetRenderContext();
	auto& pc = engine.GetPhysicsContext();
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();

	ScriptRunner scriptRunner;
	scriptRunner.RunFile("scripts/gameplay.lua");

	Scene scene;
	scene.Setup(engine);

	float accumulator = 0.0f;
	const float targetDt = 1.0f / 60.0f;

	while (engine.IsRunning()) {
		float frameTime = clock.GetDeltaTime(); // Get actual time since last frame
		accumulator += frameTime;

		engine.ProcessEvents();
		ZHLN::DrawConsole(scriptRunner);
		ZHLN::DrawProfiler(engine);
		if (!engine.IsRunning())
			break;

		if (engine.GetInput().NeedsResize()) {
			rc.SetResolution(engine.GetInput().GetNewSize());
			engine.GetInput().ClearResizeFlag();
			continue;
		}
		// Only run input-based logic if the Console/UI isn't stealing focus
		bool keyboardCaptured = ImGui::GetIO().WantCaptureKeyboard;
		// 1. Logic Phase (Lua)
		// Runs outside of locks. Lua uses the FFI bridge to queue forces/velocities.
		{
			ZHLN_PROFILE_SCOPE("Logic (Lua)");
			if (!keyboardCaptured) {
				scriptRunner.CallUpdate(&engine, frameTime);
			} else {
				// If UI is focused, we still call update but pass dt=0
				// or a flag so physics doesn't jitter, OR just skip input checks.
				scriptRunner.CallUpdate(&engine, 0.0f);
			}
		}

		MovementSystem(engine, frameTime);

		// 2. Physics Phase
		while (accumulator >= targetDt) {
			pc.Step(targetDt);
			accumulator -= targetDt;
		}

		// 3. Camera System
		if (engine.GetInput().IsMouseButtonDown(KeyCode::RButton)) {
			cam.yaw += engine.GetInput().GetMouse().deltaX * 0.2f;
			cam.pitch =
				std::clamp(cam.pitch - engine.GetInput().GetMouse().deltaY * 0.2f, -89.0f, 89.0f);
		}

		const auto& world = pc.GetWorld();
		ZHLN_LOCK(world.sync.shadowLock) {
			if (auto* pComp = reg.Get<PhysicsComponent>(scene.playerEntity)) {
				uint32_t dense = world.slotToDense[pComp->physicsHandle.index];
				JPH::Real* p = &world.positions[dense * 4];
				JPH::Vec3 target = {(float)p[0], (float)p[1] + 1.0f, (float)p[2]};

				float yR = JPH::DegreesToRadians(cam.yaw), pR = JPH::DegreesToRadians(cam.pitch);
				JPH::Vec3 dir(JPH::Cos(yR) * JPH::Cos(pR), JPH::Sin(pR),
							  JPH::Sin(yR) * JPH::Cos(pR));
				cam.position = target - (dir.Normalized() * 10.0f);
			}
		}

		// 4. Render Phase
		auto res = engine.GetWindow().GetSize();
		JPH::Mat44 vp =
			cam.GetProjectionMatrix((float)res.width / res.height) * cam.GetViewMatrix();

		Frustum frustum;
		frustum.Update(vp);

		engine.BeginFrame();
		Renderer::Clear(rc, {0.08f, 0.09f, 0.12f, 1.0f});

		ZHLN_LOCK(world.sync.shadowLock) {
			auto entities = reg.GetEntitiesWith<MeshComponent>();
			if (!entities.empty()) {
				Renderer::UpdateBuffer(
					rc, reg.Get<MeshComponent>(entities.front())->material.constantBuffer, vp);
			}

			uint32_t frameTotal = 0;
			uint32_t frameCulled = 0;

			for (Entity e : entities) {
				auto* meshComp = reg.Get<MeshComponent>(e);
				auto* physComp = reg.Get<PhysicsComponent>(e);
				frameTotal++; // Count every mesh in the ECS

				JPH::Mat44 model = JPH::Mat44::sIdentity();
				JPH::Vec3 pos = JPH::Vec3::sZero();

				if (physComp) {
					uint32_t slot = physComp->physicsHandle.index;
					uint32_t dense = world.slotToDense[slot];
					pos = {(float)world.positions[dense * 4], (float)world.positions[dense * 4 + 1],
						   (float)world.positions[dense * 4 + 2]};
				}

				// --- CULLING CHECK ---
				if (CullingStats::EnableCulling) {
					if (!frustum.IsSphereVisible(pos, meshComp->cullRadius)) {
						frameCulled++;
						continue; // Skip this object
					}
				}

				// --- TRANSFORMATION (Only happens if visible) ---
				if (physComp) {
					uint32_t slot = physComp->physicsHandle.index;
					uint32_t dense = world.slotToDense[slot];
					model = Math::CreateTransform(
						pos, {world.rotations[dense * 4], world.rotations[dense * 4 + 1],
							  world.rotations[dense * 4 + 2], world.rotations[dense * 4 + 3]});
				}

				Renderer::Draw(rc, meshComp->material, meshComp->mesh, model);
			}

			// Update global stats for ImGui to read
			CullingStats::TotalObjects = frameTotal;
			CullingStats::CulledObjects = frameCulled;
		}
		engine.EndFrame();
	}
	ZHLN::Log("Shutting down engine...");
	// Fibers shutdown automatically with RAII.
	return 0;
}