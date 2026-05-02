#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/PhysicsWorld.hpp>
#include <algorithm>
#include <lua.hpp>

using namespace ZHLN;

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
	// 1. Initialize LuaJIT
	LuaPtr L(luaL_newstate(), LuaDeleter{});
	luaL_openlibs(L.get());

	// 2. Initialize Engine
	Engine engine;
	engine.GetWindow().Focus();

	auto& renderCtx = engine.GetRenderContext();
	auto& physicsCtx = engine.GetPhysicsContext();
	auto& input = engine.GetInput();
	auto& cam = engine.GetCamera();

	// 3. Create Assets
	const Mesh floorMesh = AssetFactory::CreatePlane(renderCtx, 100.0f, {0.1f, 0.1f, 0.12f, 1.0f});
	const Mesh boxMesh =
		AssetFactory::CreateBox(renderCtx, {0.5f, 0.5f, 0.5f}, {0.8f, 0.3f, 0.2f, 1.0f});
	const Mesh playerMesh =
		AssetFactory::CreateBox(renderCtx, {0.5f, 0.9f, 0.5f}, {0.2f, 0.6f, 0.9f, 1.0f});
	const Material basic = AssetFactory::CreateBasicMaterial(renderCtx);

	// 4. Populate Physics World
	Physics::CreateStaticFloor(physicsCtx, 100.0f);

	// Create some random physics obstacles
	for (int i = 0; i < 5; ++i) {
		for (int j = 0; j < 5; ++j) {
			Physics::CreateDynamicBox(physicsCtx,
									  {(float)i * 4.0f - 10.0f, 5.0f, (float)j * 4.0f - 10.0f},
									  {0.5f, 0.5f, 0.5f});
		}
	}

	// 5. Setup Player Character
	// We use a specific handle for the player so we can retrieve them easily
	EntityHandle playerHandle = {.index = 500, .generation = 1};
	Physics::CreateCharacter(physicsCtx, {0, 2, 0}, playerHandle);

	const float dt = 1.0f / 60.0f;
	JPH::Vec3 playerPos = {0, 0, 0};

	while (engine.IsRunning()) {
        // 1. Pump OS Events
        engine.ProcessEvents();

        // 2. CRITICAL: If the user clicked 'X', stop before we try to render
        if (!engine.IsRunning()) break;

        // 3. Handle Minimization
        auto windowSize = engine.GetWindow().GetSize();
        if (windowSize.width == 0 || windowSize.height == 0) {
            // Window is minimized. Don't render, just sleep to save CPU.
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        // 4. Handle Resize (Safe from 0,0 now)
        if (input.NeedsResize()) {
            renderCtx.SetResolution(input.GetNewSize());
            input.ClearResizeFlag();
            // Important: Let the resize settle before drawing
            continue; 
        }

		// --- 6. CHARACTER CONTROL LOGIC ---
		// Determine movement direction based on Camera Yaw
		float yawRad = JPH::DegreesToRadians(cam.yaw);
		JPH::Vec3 camForward = {std::cos(yawRad), 0.0f, std::sin(yawRad)};
		JPH::Vec3 camRight = {-std::sin(yawRad), 0.0f, std::cos(yawRad)};

		JPH::Vec3 movement = {0, 0, 0};
		if (input.IsKeyDown(LLGL::Key::W))
			movement += camForward;
		if (input.IsKeyDown(LLGL::Key::S))
			movement -= camForward;
		if (input.IsKeyDown(LLGL::Key::A))
			movement -= camRight;
		if (input.IsKeyDown(LLGL::Key::D))
			movement += camRight;

		float currentSpeed = input.IsKeyDown(LLGL::Key::LShift) ? 12.0f : 5.0f;
		if (movement.LengthSq() > 0.01f) {
			Physics::SetCharacterVelocity(physicsCtx, playerHandle,
										  movement.Normalized() * currentSpeed);
		} else {
			Physics::SetCharacterVelocity(physicsCtx, playerHandle, {0, 0, 0});
		}

		// --- 7. CAMERA ORBIT LOGIC ---
		const auto& mouse = input.GetMouse();
		if (input.IsMouseButtonDown(LLGL::Key::RButton)) { // Right click to orbit
			cam.yaw += mouse.deltaX * 0.2f;
			cam.pitch -= mouse.deltaY * 0.2f;
			cam.pitch = std::clamp(cam.pitch, -40.0f, 40.0f);
		}

		// --- 8. PHYSICS STEP ---
		physicsCtx.Step(dt);

		// --- 9. CAMERA FOLLOW ---
		// We extract the player's position from the physics system to update the camera
		const auto& world = physicsCtx.GetWorld();
		{
			// Read-lock while we grab the player's position from SoA
			std::lock_guard<Mutex> lock(const_cast<Mutex&>(world.shadowLock));
			uint32_t playerDenseIdx = world.slotToDense[playerHandle.index];
			JPH::Real* p = &world.positions[playerDenseIdx * 4];
			playerPos = {(float)p[0], (float)p[1], (float)p[2]};
		}

		// Camera offset (Follow 10 units back, 5 units up)
		float dist = 10.0f;
		float pRad = JPH::DegreesToRadians(cam.pitch);
		float yRad = JPH::DegreesToRadians(cam.yaw);
		JPH::Vec3 offset = {std::cos(yRad) * std::cos(pRad) * -dist, std::sin(pRad) * -dist + 5.0f,
							std::sin(yRad) * std::cos(pRad) * -dist};
		cam.position = playerPos + offset;

		// --- 10. RENDERING ---
		engine.BeginFrame();
		Renderer::Clear(renderCtx, {0.08f, 0.09f, 0.12f, 1.0f});

		auto extent = renderCtx.GetSwapChain()->GetResolution();
		float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
		JPH::Mat44 view =
			Math::CreateLookAt(cam.position, playerPos + JPH::Vec3(0, 1, 0), JPH::Vec3::sAxisY());
		JPH::Mat44 vp = cam.GetProjectionMatrix(aspect) * view;

		// Draw Static Floor
		Renderer::Draw(renderCtx, basic, floorMesh, vp * JPH::Mat44::sIdentity());

		// Draw everything in the SoA Shadow Buffer
		{
			std::lock_guard<Mutex> lock(const_cast<Mutex&>(world.shadowLock));
			size_t activeCount = world.count.load(std::memory_order_acquire);

			for (size_t i = 0; i < activeCount; ++i) {
				if (i == 0)
					continue; // Skip floor

				JPH::Real* p = &world.positions[i * 4];
				float* r = &world.rotations[i * 4];

				JPH::Vec3 pos{(float)p[0], (float)p[1], (float)p[2]};
				JPH::Quat rot{r[0], r[1], r[2], r[3]};

				JPH::Mat44 model = Math::CreateTransform(pos, rot);

				// Identify if this dense index belongs to the player to use a different mesh
				uint32_t slotIdx = world.denseToSlot[i];
				if (slotIdx == playerHandle.index) {
					Renderer::Draw(renderCtx, basic, playerMesh, vp * model);
				} else {
					Renderer::Draw(renderCtx, basic, boxMesh, vp * model);
				}
			}
		}

		engine.EndFrame();
	}

	return 0;
}