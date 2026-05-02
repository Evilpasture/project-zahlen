#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp> // For our ZHLN::Log
#include <Zahlen/Math3D.hpp>

#include <lua.hpp>
#include <vector>

using namespace ZHLN;

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
	// 1. Initialize LuaJIT
	LuaPtr L(luaL_newstate(), LuaDeleter{});
	luaL_openlibs(L.get()); // Open print, math, string, etc.

	// 2. Test Lua Execution
	const char* luaCode = R"(
		local version = _VERSION
		local jit_status = jit and "JIT Enabled" or "JIT Disabled"
		print(string.format(">> Hello from %s (%s) inside Project-Zahlen!", version, jit_status))
		
		function calculate_rotation(time)
			return time * 0.5
		end
	)";

	if (luaL_dostring(L.get(), luaCode) != LUA_OK) {
		ZHLN::Log("Lua Error: {}\n", lua_tostring(L.get(), -1));
		return 1;
	}

	if (luaL_dofile(L.get(), "scripts/hello.lua") != LUA_OK) {
		ZHLN::Log("Lua File Error: {}\n", lua_tostring(L.get(), -1));
	}

	// 3. Initialize Engine
	Engine engine;
	engine.GetWindow().Focus();

	// Create geometry and materials.
	const Mesh floorMesh =
		AssetFactory::CreatePlane(engine.GetRenderContext(), 20.0f, {0.42f, 0.42f, 0.42f, 1.0f});
	const Mesh boxMesh = AssetFactory::CreateBox(engine.GetRenderContext(), {0.5f, 0.5f, 0.5f},
												 {0.8f, 0.3f, 0.2f, 1.0f});
	const Material basic = AssetFactory::CreateBasicMaterial(engine.GetRenderContext());

	Physics::CreateStaticFloor(engine.GetPhysicsContext(), 20.0f);
	std::vector<JPH::BodyID> boxes;
	boxes.push_back(
		Physics::CreateDynamicBox(engine.GetPhysicsContext(), {0, 8, 0}, {0.5f, 0.5f, 0.5f}));
	boxes.push_back(
		Physics::CreateDynamicBox(engine.GetPhysicsContext(), {2, 12, 0}, {0.5f, 0.5f, 0.5f}));
	boxes.push_back(
		Physics::CreateDynamicBox(engine.GetPhysicsContext(), {-2, 16, 1}, {0.5f, 0.5f, 0.5f}));

	JPH::Mat44 proj =
		Math::CreatePerspective(JPH::DegreesToRadians(45.0f), 1280.0f / 720.0f, 0.1f, 1000.0f);
	JPH::Mat44 view = Math::CreateLookAt({10, 10, 20}, {0, 2, 0}, JPH::Vec3::sAxisY());

	float time = 0.0f;

	while (engine.IsRunning()) {
		engine.ProcessEvents();
		engine.GetPhysicsContext().Step(1.0f / 60.0f);

		// 4. Verification: Call a Lua function from C++ every frame
		time += 0.016f;
		lua_getglobal(L.get(), "calculate_rotation");
		lua_pushnumber(L.get(), time);
		lua_pcall(L.get(), 1, 1, 0);
		float rotation = (float)lua_tonumber(L.get(), -1);
		lua_pop(L.get(), 1);
		JPH::Mat44 globalRotation = JPH::Mat44::sRotationY(rotation);

		engine.BeginFrame();
		Renderer::Clear(engine.GetRenderContext(), {0.1f, 0.12f, 0.15f, 1.0f});

		// Draw the ground plane.
		Renderer::Draw(engine.GetRenderContext(), basic, floorMesh,
					   proj * view * globalRotation *
						   Math::CreateTransform({0.0f, 0.0f, 0.0f}, JPH::Quat::sIdentity()));

		// Draw all dynamic physics boxes.
		for (const auto& bodyID : boxes) {
			JPH::RVec3 position = Physics::GetPosition(engine.GetPhysicsContext(), bodyID);
			JPH::Quat rotationValue = Physics::GetRotation(engine.GetPhysicsContext(), bodyID);
			JPH::Vec3 pos{static_cast<float>(position.GetX()), static_cast<float>(position.GetY()),
						  static_cast<float>(position.GetZ())};

			JPH::Mat44 model = Math::CreateTransform(pos, rotationValue);
			Renderer::Draw(engine.GetRenderContext(), basic, boxMesh, proj * view * model);
		}

		engine.EndFrame();
	}

	return 0;
}