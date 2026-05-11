#include "engine/Platform.hpp"
#include "imgui.h"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Scripting.hpp>
#include <algorithm>
#include <mutex>
#include <physics/PhysicsWorld.hpp>
#include <threading/Mutex.hpp>

using namespace ZHLN;

static void UpdatePlayerController(const InputContext& input, const Camera& cam,
								   PhysicsContext& ctx, EntityHandle player, float dt) {
	float yawRad = JPH::DegreesToRadians(cam.yaw);
	JPH::Vec3 forward = {std::cos(yawRad), 0.0f, std::sin(yawRad)};
	JPH::Vec3 right = {-std::sin(yawRad), 0.0f, std::cos(yawRad)};

	// 1. Calculate desired horizontal movement from input
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

	// 2. Retrieve existing vertical velocity from the physics object
	JPH::Vec3 currentVel = Physics::GetCharacterVelocity(ctx, player);
	float verticalVel = currentVel.GetY();

	if (Physics::IsCharacterOnGround(ctx, player)) {
		verticalVel = 0.0f;

		// Optional: Jump logic! (Holding Left Shift + Space)
		if (input.IsKeyDown(KeyCode::LShift) && input.IsKeyDown(KeyCode::Unknown)) {
			// You can map a proper Spacebar KeyCode later
		}
	} else {
		// Apply gravity manually to the vertical component
		verticalVel -= 9.81f * dt;
	}

	// 3. Combine and apply
	JPH::Vec3 finalVel = horizontalVel;
	finalVel.SetY(verticalVel);
	Physics::SetCharacterVelocity(ctx, player, finalVel);
}

struct Scene {
	Mesh floor;
	Mesh box;
	Mesh player;
	Material material;
	EntityHandle playerHandle;

	void Setup(RenderContext& rc, PhysicsContext& pc) {
		// --- Render Meshes Setup ---
		floor = AssetFactory::CreatePlane(rc, 100.0f, {0.1f, 0.1f, 0.12f, 1.0f});
		box = AssetFactory::CreateBox(rc, {0.5f, 0.5f, 0.5f}, {0.8f, 0.3f, 0.2f, 1.0f});
		player = AssetFactory::CreateBox(rc, {0.5f, 0.9f, 0.5f}, {0.2f, 0.6f, 0.9f, 1.0f});
		material = AssetFactory::CreateBasicMaterial(rc);

		// --- Physics Setup ---
		// 1. Get Cached Shapes (Zero Heap Allocations on reuse!)
		auto floorShape =
			Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 100.0f, 1.0f, 100.0f);
		auto boxShape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 0.5f, 0.5f, 0.5f);

		// 2. Spawn Static Floor (Engine automatically assigns index 0)
		// Layer 0 = Static
		Physics::CreateRigidBody(pc, floorShape, {0.0f, -1.0f, 0.0f}, JPH::Quat::sIdentity(),
								 JPH::EMotionType::Static, 0);

		// 3. Spawn a Wall of Props
		// Layer 1 = Dynamic
		for (int row = 0; row < 5; ++row) {
			for (int col = 0; col < 5; ++col) {
				Physics::CreateRigidBody(pc, boxShape,
										 {(float)col - 2.5f, (float)row + 0.5f, -5.0f},
										 JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, 1);
			}
		}

		// 4. Setup Player (Capture the handle the engine creates!)
		playerHandle = Physics::CreateCharacter(pc, {0.0f, 2.0f, 0.0f});
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

	ScriptRunner scriptRunner;
	scriptRunner.RunFile("scripts/gameplay.lua");

	Scene scene;
	scene.Setup(rc, pc);

	const float dt = 1.0f / 60.0f;

	while (engine.IsRunning()) {
		engine.ProcessEvents();
		if (!engine.IsRunning())
			break;

		auto res = engine.GetWindow().GetSize();
		if (res.width == 0 || res.height == 0) {
			ZHLN::Log("Window size is 0,0. Waiting...");
			Platform::Sleep(100);
			continue;
		}

		if (input.NeedsResize()) {
			rc.SetResolution(input.GetNewSize());
			input.ClearResizeFlag();
			continue;
		}

		UpdatePlayerController(input, cam, pc, scene.playerHandle, dt);

		// --- SCRIPTING UPDATE ---
		scriptRunner.CallUpdate(&engine, dt);

		// Handle Camera Orbit (Right Click Drag)
		if (input.IsMouseButtonDown(KeyCode::RButton)) {
			cam.yaw += input.GetMouse().deltaX * 0.2f;
			cam.pitch = std::clamp(cam.pitch - input.GetMouse().deltaY * 0.2f, -89.0f, 89.0f);
		}

		pc.Step(dt);

		// 1. Sync Physics & Compute Camera Position
		const auto& world = pc.GetWorld();
		JPH::Vec3 pPos;
		{
			std::lock_guard lock(const_cast<Mutex&>(world.shadowLock));

			// Use the Engine's SoA mapping to safely find the player's memory!
			uint32_t idx = world.slotToDense[scene.playerHandle.index];
			JPH::Real* p = &world.positions[idx * 4];
			pPos = {(float)p[0], (float)p[1], (float)p[2]};
		}

		// Compute Orbit Position
		float yR = JPH::DegreesToRadians(cam.yaw);
		float pR = JPH::DegreesToRadians(cam.pitch);
		JPH::Vec3 direction(JPH::Cos(yR) * JPH::Cos(pR), JPH::Sin(pR), JPH::Sin(yR) * JPH::Cos(pR));
		JPH::Vec3 target = pPos + JPH::Vec3(0.0f, 1.0f, 0.0f);

		// Camera pushes backward out from the target along the direction
		cam.position = target - (direction.Normalized() * 10.0f);

		// 2. Prepare Frame
		JPH::Mat44 vp =
			cam.GetProjectionMatrix((float)res.width / res.height) * cam.GetViewMatrix();
		Renderer::UpdateBuffer(rc, scene.material.constantBuffer, vp);

		ImGui::Begin("Zahlen Debug");
		ImGui::Text("Physics Objects: %zu", pc.GetWorld().count.load());
		if (ImGui::Button("Reset Camera")) {
			cam.position = {0, 2, 10};
		}
		ImGui::End();

		// 3. Render
		engine.BeginFrame();
		Renderer::Clear(rc, {0.08f, 0.09f, 0.12f, 1.0f});

		// Draw static floor manually (It is at the world origin)
		Renderer::Draw(rc, scene.material, scene.floor, JPH::Mat44::sIdentity());

		{
			std::lock_guard lock(const_cast<Mutex&>(world.shadowLock));

			// Loop over the dense array.
			// We start at i=1 because i=0 is the floor we just drew.
			for (size_t i = 1; i < world.count.load(std::memory_order_acquire); ++i) {
				JPH::Real* p = &world.positions[i * 4];
				float* r = &world.rotations[i * 4];

				JPH::Mat44 model = Math::CreateTransform({(float)p[0], (float)p[1], (float)p[2]},
														 {r[0], r[1], r[2], r[3]});

				// Select Mesh based on whether this slot belongs to the player
				Mesh& meshToDraw =
					(world.denseToSlot[i] == scene.playerHandle.index) ? scene.player : scene.box;

				Renderer::Draw(rc, scene.material, meshToDraw, model);
			}
		}
		engine.EndFrame();
	}
	return 0;
}