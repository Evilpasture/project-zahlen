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

using namespace ZHLN;

// Updated to take the ECS Entity, then fetch its physics handle
static void UpdatePlayerController(const InputContext& input, const Camera& cam,
								   PhysicsContext& ctx, ECS::Registry& reg,
								   ZHLN::Entity playerEntity, float dt) {

	// Grab the Physics Component from the ECS
	auto* physComp = reg.Get<PhysicsComponent>(playerEntity);
	if (!physComp)
		return;

	ZHLN::Entity physHandle = physComp->physicsHandle;

	float yawRad = JPH::DegreesToRadians(cam.yaw);
	JPH::Vec3 forward = {std::cos(yawRad), 0.0f, std::sin(yawRad)};
	JPH::Vec3 right = {-std::sin(yawRad), 0.0f, std::cos(yawRad)};

	JPH::Vec3 move = JPH::Vec3::sZero();
	if (input.IsKeyDown(KeyCode::W))
		move += forward;
	if (input.IsKeyDown(KeyCode::S))
		move -= forward;
	if (input.IsKeyDown(KeyCode::A))
		move -= right;
	if (input.IsKeyDown(KeyCode::D))
		move += right;

	float speed = input.IsKeyDown(KeyCode::LShift) ? 12.0f : 5.0f;
	JPH::Vec3 horizontalVel =
		(move.LengthSq() > 0.01f) ? move.Normalized() * speed : JPH::Vec3::sZero();

	JPH::Vec3 currentVel = Physics::GetCharacterVelocity(ctx, physHandle);
	float verticalVel = currentVel.GetY();

	if (Physics::IsCharacterOnGround(ctx, physHandle)) {
		verticalVel = 0.0f;
	} else {
		verticalVel -= 9.81f * dt;
	}

	JPH::Vec3 finalVel = horizontalVel;
	finalVel.SetY(verticalVel);
	Physics::SetCharacterVelocity(ctx, physHandle, finalVel);
}

struct Scene {
	ZHLN::Entity playerEntity;

	void Setup(Engine& engine) {
		auto& rc = engine.GetRenderContext();
		auto& pc = engine.GetPhysicsContext();
		auto& reg = engine.GetRegistry();

		// Ensure components are registered (allocates sparse sets)
		reg.RegisterComponent<MeshComponent>();
		reg.RegisterComponent<PhysicsComponent>();

		// --- Cache Assets ---
		Mesh floorMesh = AssetFactory::CreatePlane(rc, 100.0f, {0.1f, 0.1f, 0.12f, 1.0f});
		Mesh boxMesh = AssetFactory::CreateBox(rc, {0.5f, 0.5f, 0.5f}, {0.8f, 0.3f, 0.2f, 1.0f});
		Mesh playerMesh = AssetFactory::CreateBox(rc, {0.5f, 0.9f, 0.5f}, {0.2f, 0.6f, 0.9f, 1.0f});
		Material material = AssetFactory::CreateBasicMaterial(rc);

		auto floorShape =
			Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 100.0f, 1.0f, 100.0f);
		auto boxShape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 0.5f, 0.5f, 0.5f);

		// --- Spawn Floor ---
		Entity floorPhys =
			Physics::CreateRigidBody(pc, floorShape, {0.0f, -1.0f, 0.0f}, JPH::Quat::sIdentity(),
									 JPH::EMotionType::Static, 0);
		Entity floorEnt = reg.Create();
		reg.Add(floorEnt, MeshComponent{floorMesh, material});
		reg.Add(floorEnt, PhysicsComponent{floorPhys});

		// --- Spawn Wall of Props ---
		for (int row = 0; row < 5; ++row) {
			for (int col = 0; col < 5; ++col) {
				Entity propPhys = Physics::CreateRigidBody(
					pc, boxShape, {(float)col - 2.5f, (float)row + 0.5f, -5.0f},
					JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, 1);

				Entity propEnt = reg.Create();
				reg.Add(propEnt, MeshComponent{boxMesh, material});
				reg.Add(propEnt, PhysicsComponent{propPhys});
			}
		}

		// --- Spawn Player ---
		Entity playerPhys = Physics::CreateCharacter(pc, {0.0f, 2.0f, 0.0f});
		playerEntity = reg.Create();
		reg.Add(playerEntity, MeshComponent{playerMesh, material});
		reg.Add(playerEntity, PhysicsComponent{playerPhys});
	}
};

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
	Platform::Init();
	Engine engine;
	engine.GetWindow().Focus();

	auto& rc = engine.GetRenderContext();
	auto& pc = engine.GetPhysicsContext();
	auto& input = engine.GetInput();
	auto& cam = engine.GetCamera();
	auto& reg = engine.GetRegistry();

	ScriptRunner scriptRunner;
	scriptRunner.RunFile("scripts/gameplay.lua");

	Scene scene;
	scene.Setup(engine);

	const float dt = 1.0f / 60.0f;

	while (engine.IsRunning()) {
		engine.ProcessEvents();
		if (!engine.IsRunning())
			break;

		auto res = engine.GetWindow().GetSize();
		if (res.width == 0 || res.height == 0) {
			Platform::Sleep(100);
			continue;
		}

		if (input.NeedsResize()) {
			rc.SetResolution(input.GetNewSize());
			input.ClearResizeFlag();
			continue;
		}

		UpdatePlayerController(input, cam, pc, reg, scene.playerEntity, dt);
		scriptRunner.CallUpdate(&engine, dt);

		if (input.IsMouseButtonDown(KeyCode::RButton)) {
			cam.yaw += input.GetMouse().deltaX * 0.2f;
			cam.pitch = std::clamp(cam.pitch - input.GetMouse().deltaY * 0.2f, -89.0f, 89.0f);
		}

		pc.Step(dt);

		// 1. Sync Camera Position using the ECS
		const auto& world = pc.GetWorld();
		ZHLN_LOCK(world.shadowLock) {
			if (auto* pComp = reg.Get<PhysicsComponent>(scene.playerEntity)) {
				uint32_t slot = pComp->physicsHandle.index;
				uint32_t dense = world.slotToDense[slot];
				JPH::Real* p = &world.positions[dense * 4];

				JPH::Vec3 target = {(float)p[0], (float)p[1] + 1.0f, (float)p[2]};
				float yR = JPH::DegreesToRadians(cam.yaw);
				float pR = JPH::DegreesToRadians(cam.pitch);
				JPH::Vec3 dir(JPH::Cos(yR) * JPH::Cos(pR), JPH::Sin(pR),
							  JPH::Sin(yR) * JPH::Cos(pR));

				cam.position = target - (dir.Normalized() * 10.0f);
			}
		}

		JPH::Mat44 vp =
			cam.GetProjectionMatrix((float)res.width / res.height) * cam.GetViewMatrix();

		ImGui::Begin("Zahlen Debug");
		ImGui::Text("Active Entities: %zu", reg.GetEntitiesWith<MeshComponent>().size());
		ImGui::End();

		engine.BeginFrame();
		Renderer::Clear(rc, {0.08f, 0.09f, 0.12f, 1.0f});

		// 2. The new ECS-Driven Render Loop
		ZHLN_LOCK(world.shadowLock) {
			// Update global VP buffer once
			// In a real engine, we'd only do this for the one material we have,
			// but for simplicity, we grab the material from the first entity
			auto entities = reg.GetEntitiesWith<MeshComponent>();
			if (!entities.empty()) {
				Renderer::UpdateBuffer(
					rc, reg.Get<MeshComponent>(entities.front())->material.constantBuffer, vp);
			}

			// Iterate over everything that has a Mesh
			for (Entity e : entities) {
				auto* meshComp = reg.Get<MeshComponent>(e);
				auto* physComp = reg.Get<PhysicsComponent>(e);

				JPH::Mat44 model = JPH::Mat44::sIdentity();

				// If it has physics, sync the transform directly from the SoA memory!
				if (physComp) {
					uint32_t slot = physComp->physicsHandle.index;
					if (world.generations[slot].load(std::memory_order_relaxed) ==
						physComp->physicsHandle.generation) {
						uint32_t dense = world.slotToDense[slot];
						JPH::Real* p = &world.positions[dense * 4];
						float* r = &world.rotations[dense * 4];
						model = Math::CreateTransform({(float)p[0], (float)p[1], (float)p[2]},
													  {r[0], r[1], r[2], r[3]});
					}
				}

				Renderer::Draw(rc, meshComp->material, meshComp->mesh, model);
			}
		}

		engine.EndFrame();
	}
	return 0;
}