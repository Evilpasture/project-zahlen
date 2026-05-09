#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp> // Added for timing
#include <Zahlen/Engine.hpp>
#include <Zahlen/PhysicsWorld.hpp>
#include <Zahlen/Platform.hpp>

using namespace ZHLN;

auto main() -> int {
	Platform::Init();
	Engine engine;
    ZHLN::Math::TestMathStack();
	engine.GetWindow().Focus();

	auto& rc = engine.GetRenderContext();
	auto& pc = engine.GetPhysicsContext();
	auto& input = engine.GetInput();
	auto& cam = engine.GetCamera();
	Clock clock; // Engine clock to measure real time

	// --- Asset & Physics Initialization ---
	Mesh floorMesh = AssetFactory::CreatePlane(rc, 100.0f, {1, 1, 1, 1});
	Mesh boxMesh = AssetFactory::CreateBox(rc, {0.5f, 0.5f, 0.5f}, {1, 1, 1, 1});
	Material material = AssetFactory::CreateBasicMaterial(rc);

	Physics::CreateStaticFloor(pc, 100.0f, {.index = 0, .generation = 1});
	for (uint32_t i = 0; i < 25; ++i) {
		JPH::RVec3 pos((float)(i % 5) * 4.0f - 10.0f, 5.0f, (float)(i / 5.0f) * 4.0f - 10.0f);
		Physics::CreateDynamicBox(pc, pos, {0.5f, 0.5f, 0.5f}, {.index = i + 1, .generation = 1});
	}
	EntityHandle player = Physics::CreateCharacter(pc, {0, 2, 0}, {.index = 500, .generation = 1});

	// --- Timing Configuration ---
	const float physicsRate = 1.0f / 60.0f; // Fixed 60Hz update
	float accumulator = 0.0f;

	while (engine.IsRunning()) {
		engine.ProcessEvents();
		if (!engine.IsRunning()) break;
		if (engine.GetWindow().GetSize().width == 0) { Platform::Sleep(16); continue; }

		// --- 1. Timing & Physics (The Accumulator Fix) ---
		float frameTime = clock.GetDeltaTime();
		if (frameTime > 0.25f) frameTime = 0.25f; // "Spiral of death" protection
		accumulator += frameTime;

		// --- 2. Input & Character Logic ---
		const auto& mouse = input.GetMouse();
		
		// Fix Inversion:
		// Right is usually +, Up is usually - in screen space. 
		// We add to Yaw and subtract from Pitch to match the Camera.hpp math.
		cam.yaw   += mouse.deltaX * 0.15f;
		cam.pitch -= mouse.deltaY * 0.15f; 
		cam.pitch = JPH::Clamp(cam.pitch, -89.0f, 89.0f);

		// Calculate movement vectors matching the Camera's View Matrix
		float radYaw = JPH::DegreesToRadians(cam.yaw);
		JPH::Vec3 forward(JPH::Cos(radYaw), 0, JPH::Sin(radYaw));
		JPH::Vec3 right(JPH::Sin(radYaw), 0, -JPH::Cos(radYaw));

		JPH::Vec3 moveDir = JPH::Vec3::sZero();
		if (input.IsKeyDown(KeyCode::W)) moveDir += forward;
		if (input.IsKeyDown(KeyCode::S)) moveDir -= forward;
		if (input.IsKeyDown(KeyCode::A)) moveDir -= right;
		if (input.IsKeyDown(KeyCode::D)) moveDir += right;

		if (moveDir.LengthSq() > 0.001f) {
			Physics::SetCharacterVelocity(pc, player, moveDir.Normalized() * 8.0f);
		} else {
			Physics::SetCharacterVelocity(pc, player, JPH::Vec3::sZero());
		}

		// Step physics at a fixed rate, regardless of framerate
		while (accumulator >= physicsRate) {
			pc.Step(physicsRate);
			accumulator -= physicsRate;
		}

		// --- 3. Data-Oriented Extraction & Render ---
		const auto& world = pc.GetWorld();
		{
			std::lock_guard lock(const_cast<Mutex&>(world.shadowLock));

			// Extract Player position for Camera (with 1:1 sync)
			uint32_t playerDense = world.slotToDense[player.index];
			JPH::Real* pPos = &world.positions[playerDense * 4];
			cam.position = { (float)pPos[0], (float)pPos[1] + 0.9f, (float)pPos[2] };

			auto res = engine.GetWindow().GetSize();
			JPH::Mat44 vp = cam.GetProjectionMatrix((float)res.width / res.height) * cam.GetViewMatrix();
			rc.SetCamera(vp, cam.position);
			rc.SetSunlight({-0.5f, -1.0f, 0.5f}, {1.0f, 0.95f, 0.8f}, 4.0f);

			engine.BeginFrame();
			for (size_t i = 0; i < world.count.load(); ++i) {
				JPH::Real* p = &world.positions[i * 4];
				float* r = &world.rotations[i * 4];
				JPH::Mat44 transform = Math::CreateTransform({(float)p[0], (float)p[1], (float)p[2]}, {r[0], r[1], r[2], r[3]});

				if (i == 0) Renderer::Draw(rc, material, floorMesh, transform);
				else        Renderer::Draw(rc, material, boxMesh, transform);
			}
			engine.EndFrame(); 
		}
	}
	return 0;
}