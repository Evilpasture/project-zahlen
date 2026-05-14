#include "engine/Platform.hpp"
#include "imgui.h"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Scripting.hpp>
#include <algorithm>
#include <detail/ControlFlow.hpp>
#include <physics/PhysicsWorld.hpp>
#include <threading/Mutex.hpp>
#include <threading/TaskSystem.hpp>

using namespace ZHLN;

struct Scene {
	ZHLN::Entity playerEntity;

	void Setup(Engine& engine) {
		auto& rc = engine.GetRenderContext();
		auto& pc = engine.GetPhysicsContext();
		auto& reg = engine.GetRegistry();

		reg.RegisterComponent<MeshComponent>();
		reg.RegisterComponent<PhysicsComponent>();

		// --- Assets ---
		Mesh floorMesh = AssetFactory::CreatePlane(rc, 100.0f, {0.1f, 0.1f, 0.12f, 1.0f});
		Mesh boxMesh = AssetFactory::CreateBox(rc, {0.5f, 0.5f, 0.5f}, {0.8f, 0.3f, 0.2f, 1.0f});
		Mesh playerMesh = AssetFactory::CreateBox(rc, {0.5f, 0.9f, 0.5f}, {0.2f, 0.6f, 0.9f, 1.0f});
		Material material = AssetFactory::CreateBasicMaterial(rc);

		auto floorShape =
			Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 100.0f, 1.0f, 100.0f);
		auto boxShape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 0.5f, 0.5f, 0.5f);

		// --- Spawn ---
		reg.Add(reg.Create(), MeshComponent{floorMesh, material},
				PhysicsComponent{Physics::CreateRigidBody(pc, floorShape, {0.0f, -1.0f, 0.0f},
														  JPH::Quat::sIdentity(),
														  JPH::EMotionType::Static, 0)});

		for (int i = 0; i < 2000; ++i) {
			float x = (float)(i % 45) - 22.5f;
			float z = (float)(i / 45.0f) - 22.5f;
			Entity propPhys =
				Physics::CreateRigidBody(pc, boxShape, {x, 5.0f + (i * 0.1f), z},
										 JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, 1);
			reg.Add(reg.Create(), MeshComponent{boxMesh, material}, PhysicsComponent{propPhys});
		}

		playerEntity = reg.Create();
		reg.Add(playerEntity, MeshComponent{playerMesh, material});
		reg.Add(playerEntity, PhysicsComponent{Physics::CreateCharacter(pc, {0.0f, 2.0f, 0.0f})});
	}
};

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
	Platform::Init();
	ZHLN::SetupSignalHandler();
	TaskSystem::Init();
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

	const float dt = 1.0f / 60.0f;

	while (engine.IsRunning()) {
		engine.ProcessEvents();
		if (!engine.IsRunning())
			break;

		if (engine.GetInput().NeedsResize()) {
			rc.SetResolution(engine.GetInput().GetNewSize());
			engine.GetInput().ClearResizeFlag();
			continue;
		}

		// 1. Logic Phase (Lua)
		// Runs outside of locks. Lua uses the FFI bridge to queue forces/velocities.
		scriptRunner.CallUpdate(&engine, dt);

		// 2. Physics Phase
		pc.Step(dt);

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

		engine.BeginFrame();
		Renderer::Clear(rc, {0.08f, 0.09f, 0.12f, 1.0f});

		ZHLN_LOCK(world.sync.shadowLock) {
			auto entities = reg.GetEntitiesWith<MeshComponent>();
			if (!entities.empty()) {
				Renderer::UpdateBuffer(
					rc, reg.Get<MeshComponent>(entities.front())->material.constantBuffer, vp);
			}

			for (Entity e : entities) {
				auto* meshComp = reg.Get<MeshComponent>(e);
				auto* physComp = reg.Get<PhysicsComponent>(e);
				JPH::Mat44 model = JPH::Mat44::sIdentity();

				if (physComp) {
					uint32_t slot = physComp->physicsHandle.index;
					uint32_t dense = world.slotToDense[slot];
					model = Math::CreateTransform(
						{(float)world.positions[dense * 4], (float)world.positions[dense * 4 + 1],
						 (float)world.positions[dense * 4 + 2]},
						{world.rotations[dense * 4], world.rotations[dense * 4 + 1],
						 world.rotations[dense * 4 + 2], world.rotations[dense * 4 + 3]});
				}
				Renderer::Draw(rc, meshComp->material, meshComp->mesh, model);
			}
		}
		engine.EndFrame();
	}
	ZHLN::Log("Shutting down engine...");
	// Fibers shutdown automatically with RAII.
	return 0;
}