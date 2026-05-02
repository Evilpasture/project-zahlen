#include "engine/AssetFactory.hpp"
#include "engine/Engine.hpp"
#include "engine/Log.hpp" // For our ZHLN::Log
#include "engine/Math3D.hpp"

#include <lua.hpp>

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

	// ... (Assets and Physics setup remain the same) ...
	const Mesh tetra = AssetFactory::CreateTetrahedron(engine.GetRenderContext());
	const Material basic = AssetFactory::CreateBasicMaterial(engine.GetRenderContext());
	JPH::BodyID box =
		Physics::CreateDynamicBox(engine.GetPhysicsContext(), {0, 10, 0}, {0.5, 0.5, 0.5});

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

		engine.BeginFrame();
		Renderer::Clear(engine.GetRenderContext(), {0.1f, 0.12f, 0.15f, 1.0f});

		// Apply the rotation calculated by Lua
		JPH::Mat44 model = JPH::Mat44::sRotationY(rotation) *
						   JPH::Mat44::sRotationTranslation(
							   Physics::GetRotation(engine.GetPhysicsContext(), box),
							   JPH::Vec3(Physics::GetPosition(engine.GetPhysicsContext(), box)));

		Renderer::Draw(engine.GetRenderContext(), basic, tetra, proj * view * model);

		engine.EndFrame();
	}

	return 0;
}