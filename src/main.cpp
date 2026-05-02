#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/PhysicsWorld.hpp> // Access to SoA struct
#include <lua.hpp>

using namespace ZHLN;

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
	// 1. Initialize LuaJIT
	LuaPtr L(luaL_newstate(), LuaDeleter{});
	luaL_openlibs(L.get());

	const char* luaCode = R"(
		function calculate_rotation(time)
			return time * 0.2
		end
	)";
	luaL_dostring(L.get(), luaCode);

	// 2. Initialize Engine
	Engine engine;
	engine.GetWindow().Focus();

	auto& renderCtx = engine.GetRenderContext();
	auto& physicsCtx = engine.GetPhysicsContext();
	auto& input = engine.GetInput();
	auto& cam = engine.GetCamera();

	// 3. Create Assets
	const Mesh floorMesh = AssetFactory::CreatePlane(renderCtx, 50.0f, {0.2f, 0.2f, 0.22f, 1.0f});
	const Mesh boxMesh =
		AssetFactory::CreateBox(renderCtx, {0.5f, 0.5f, 0.5f}, {0.8f, 0.3f, 0.2f, 1.0f});
	const Material basic = AssetFactory::CreateBasicMaterial(renderCtx);

	// 4. Populate Physics World
	Physics::CreateStaticFloor(physicsCtx, 50.0f);
	for (int i = 0; i < 10; ++i) {
		for (int j = 0; j < 10; ++j) {
			Physics::CreateDynamicBox(physicsCtx,
									  {(float)i - 5.0f, 10.0f + (j * 2.0f), (float)j - 5.0f},
									  {0.5f, 0.5f, 0.5f});
		}
	}

	float time = 0.0f;
	const float dt = 1.0f / 60.0f;

	while (engine.IsRunning()) {
		// Update OS events and Input deltas
		engine.ProcessEvents();

		// --- 5. FLY-CAM LOGIC ---
		float moveSpeed = 15.0f * dt;
		if (input.IsKeyDown(LLGL::Key::LShift))
			moveSpeed *= 3.0f;

		// Calculate camera direction vectors
		float yawRad = JPH::DegreesToRadians(cam.yaw);
		float pitchRad = JPH::DegreesToRadians(cam.pitch);
		JPH::Vec3 forward(std::cos(yawRad) * std::cos(pitchRad), std::sin(pitchRad),
						  std::sin(yawRad) * std::cos(pitchRad));
		forward = forward.Normalized();
		JPH::Vec3 right = forward.Cross(JPH::Vec3::sAxisY()).Normalized();

		if (input.IsKeyDown(LLGL::Key::W))
			cam.position += forward * moveSpeed;
		if (input.IsKeyDown(LLGL::Key::S))
			cam.position -= forward * moveSpeed;
		if (input.IsKeyDown(LLGL::Key::A))
			cam.position -= right * moveSpeed;
		if (input.IsKeyDown(LLGL::Key::D))
			cam.position += right * moveSpeed;

		// Rotate camera with Mouse Left-Click
		const auto& mouse = input.GetMouse();
		if (input.IsMouseButtonDown(LLGL::Key::LButton)) {
			cam.yaw += mouse.deltaX * 0.15f;
			cam.pitch -= mouse.deltaY * 0.15f;
			cam.pitch = std::clamp(cam.pitch, -89.0f, 89.0f);
		}

		// --- 6. PHYSICS STEP ---
		physicsCtx.Step(dt);

		// --- 7. LUA LOGIC ---
		time += dt;
		lua_getglobal(L.get(), "calculate_rotation");
		lua_pushnumber(L.get(), time);
		lua_pcall(L.get(), 1, 1, 0);
		float rotation = (float)lua_tonumber(L.get(), -1);
		lua_pop(L.get(), 1);
		JPH::Mat44 globalRot = JPH::Mat44::sRotationY(rotation);

		// --- 8. RENDERING ---
		engine.BeginFrame();
		Renderer::Clear(renderCtx, {0.1f, 0.12f, 0.15f, 1.0f});

		auto extent = renderCtx.GetSwapChain()->GetResolution();
		float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
		JPH::Mat44 vp = cam.GetProjectionMatrix(aspect) * cam.GetViewMatrix() * globalRot;

		// Draw Ground
		Renderer::Draw(renderCtx, basic, floorMesh,
					   vp * Math::CreateTransform({0, 0, 0}, JPH::Quat::sIdentity()));

		// --- 9. SOA SHADOW BUFFER RENDERING ---
		// We lock the shadow buffer to ensure the Physics Sync pass isn't writing while we read.
		const auto& world = physicsCtx.GetWorld();
		{
			// shadowLock is a member of the World POD
			std::lock_guard<Mutex> lock(const_cast<Mutex&>(world.shadowLock));

			size_t activeCount = world.count.load(std::memory_order_acquire);
			for (size_t i = 0; i < activeCount; ++i) {
				// Index 0 is the floor
				if (i == 0)
					continue;

				JPH::Real* p = &world.positions[i * 4];
				float* r = &world.rotations[i * 4];

				JPH::Vec3 pos{(float)p[0], (float)p[1], (float)p[2]};
				JPH::Quat rot{r[0], r[1], r[2], r[3]};

				JPH::Mat44 model = Math::CreateTransform(pos, rot);
				Renderer::Draw(renderCtx, basic, boxMesh, vp * model);
			}
		}

		engine.EndFrame();
	}

	return 0;
}