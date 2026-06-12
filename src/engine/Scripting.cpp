// src/engine/Scripting.cpp

#include "Zahlen/Camera.hpp"
#include "Zahlen/Input.hpp"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Console.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Scripting.h>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/physics/Physics_C.h>
#include <chrono>
#include <cstring>
#include <functional>
#include <physics/PhysicsWorld.hpp>
#include <print>
#include <string_view>
#include <threading/Channel.hpp>
#include <unordered_map>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

// Local, library-allocated value state (No exported pointers!)
static ZHLN_GameState s_LocalGameState{};

// Opaque declarations of ConsoleUI globals (defined in ConsoleUI.cpp)
extern std::vector<std::string> s_InvShellLog;
extern bool s_InvScrollToBottom;

#pragma pack(push, 1)
struct SpawnPrefabArgs {
	char path[256];
	float px, py, pz;
	int createPhysics;
	int isStatic;
	int isAnimated;
	uint32_t maxCount;
	uint64_t* outEntities;
};

struct SetupRagdollArgs {
	uint64_t playerEntity;
	uint32_t count;
	uint64_t* visualParts;
};

struct CreateBoxArgs {
	float hx, hy, hz;
	float r, g, b, a;
};

struct CreateMaterialArgs {
	float r, g, b, a;
	uint64_t* outPipeline;
	uint32_t* outAlbedo;
};
#pragma pack(pop)

using CommandHandler = std::function<uint64_t(ZHLN::Engine*, const void*)>;
static std::unordered_map<std::string_view, CommandHandler> s_CommandRegistry;

static void RegisterFFICommands() {
	if (!s_CommandRegistry.empty())
		return;

	s_CommandRegistry["SpawnPrefab"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		auto* a = static_cast<const SpawnPrefabArgs*>(args);
		auto& rc = engine->GetRenderContext();
		auto& reg = engine->GetRegistry();
		auto& pc = engine->GetPhysicsContext();

		auto* prefab = ZHLN::AssetFactory::LoadModelPrefab(rc, engine->GetAssetManager(), a->path);
		if (!prefab) {
			return 0;
		}

		ZHLN::AssetFactory::SpawnParams params;
		params.position = JPH::RVec3(a->px, a->py, a->pz);
		params.createPhysics = a->createPhysics != 0;
		params.isStaticPhysics = a->isStatic != 0;
		params.isAnimated = a->isAnimated != 0;
		params.useBoxColliders = false;

		std::vector<ZHLN::Entity> temp_buffer(a->maxCount);
		uint32_t count = ZHLN::AssetFactory::InstantiatePrefab(rc, reg, pc, *prefab, params,
															   temp_buffer.data(), a->maxCount);

		uint32_t writtenCount = std::min(count, a->maxCount);
		for (uint32_t i = 0; i < writtenCount; ++i) {
			a->outEntities[i] = temp_buffer[i].Pack();
		}
		return writtenCount;
	};

	s_CommandRegistry["SetupRagdoll"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		auto* a = static_cast<const SetupRagdollArgs*>(args);
		auto& rc = engine->GetRenderContext();
		auto& pc = engine->GetPhysicsContext();
		auto& reg = engine->GetRegistry();

		std::vector<ZHLN::Entity> parts(a->count);
		for (uint32_t i = 0; i < a->count; ++i) {
			parts[i] = ZHLN::Entity::Unpack(a->visualParts[i]);
		}

		ZHLN::AssetFactory::SetupPlayerRagdoll(rc, pc, reg, ZHLN::Entity::Unpack(a->playerEntity),
											   parts);
		return 1;
	};

	s_CommandRegistry["CreateBox"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		auto* a = static_cast<const CreateBoxArgs*>(args);
		auto& rc = engine->GetRenderContext();
		ZHLN::Mesh mesh = ZHLN::AssetFactory::CreateBox(rc, JPH::Vec3(a->hx, a->hy, a->hz),
														JPH::Vec4(a->r, a->g, a->b, a->a));
		return static_cast<uint64_t>(mesh.vertexBuffer);
	};

	s_CommandRegistry["CreateBasicMaterial"] = [](ZHLN::Engine* engine,
												  const void* args) -> uint64_t {
		auto* a = static_cast<const CreateMaterialArgs*>(args);
		auto& rc = engine->GetRenderContext();
		ZHLN::Material mat = ZHLN::AssetFactory::CreateBasicMaterial(rc);
		*a->outPipeline = static_cast<uint64_t>(mat.pipeline);
		*a->outAlbedo = mat.albedoIndex;
		return 1;
	};
}

struct ZHLN_LuaChannel {
	ZHLN::Channel<int> channel;
};

// ============================================================================
// ALL C-EXPORTS MUST BE INSIDE THIS BLOCK TO AVOID MANGLING
// ============================================================================
extern "C" {

ZHLN_LuaChannel* ZHLN_CreateLuaChannel(void) {
	return new ZHLN_LuaChannel();
}

void ZHLN_DestroyLuaChannel(ZHLN_LuaChannel* chan) {
	delete chan;
}

void ZHLN_PushLuaChannel(ZHLN_Engine* /*engine*/, ZHLN_LuaChannel* chan, lua_State* L) {
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	chan->channel.Push(ref);
}

void ZHLN_PopLuaChannel(ZHLN_Engine* /*engine*/, ZHLN_LuaChannel* chan, lua_State* L) {
	int ref = chan->channel.Pop();
	lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
	luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

ZHLN_Engine* ZHLN_GetEngineContext() {
	return reinterpret_cast<ZHLN_Engine*>(ZHLN::GetEngineContext());
}

void ZHLN_SetGameState(ZHLN_Engine* /*engine_handle*/, const ZHLN_GameState* state_ptr) {
	if (state_ptr != nullptr) {
		s_LocalGameState = *state_ptr;
	}
}

void* ZHLN_GetGameState(ZHLN_Engine* /*engine_handle*/) {
	return &s_LocalGameState;
}

uint64_t ZHLN_DispatchCommand(ZHLN_Engine* engine_handle, const char* cmd, const void* args) {
	RegisterFFICommands();
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	auto it = s_CommandRegistry.find(std::string_view(cmd));
	if (it != s_CommandRegistry.end()) {
		return it->second(engine, args);
	}
	return 0;
}

float ZHLN_GetTotalTime(ZHLN_Engine* /*engine_handle*/) {
	static auto start = std::chrono::high_resolution_clock::now();
	auto now = std::chrono::high_resolution_clock::now();
	return std::chrono::duration<float>(now - start).count();
}

int ZHLN_IsKeyDown(ZHLN_Engine* engine_handle, uint8_t key) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	return engine->GetInput().IsKeyDown(static_cast<ZHLN::KeyCode>(key)) ? 1 : 0;
}

void ZHLN_GetMouseDelta(ZHLN_Engine* engine_handle, float* outX, float* outY) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	*outX = engine->GetInput().GetMouse().deltaX;
	*outY = engine->GetInput().GetMouse().deltaY;
}

float ZHLN_GetCameraYaw(ZHLN_Engine* engine_handle) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	return engine->GetCamera().yaw;
}

float ZHLN_GetCameraFOV(ZHLN_Engine* engine_handle) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	return engine->GetCamera().fov;
}

void ZHLN_SetCameraFOV(ZHLN_Engine* engine_handle, float fov) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	engine->GetCamera().fov = fov;
}

void ZHLN_LogInventoryShell(const char* msg) {
	std::string str(msg);
	size_t pos = 0;

	while (pos < str.size()) {
		size_t next_nl = str.find('\n', pos);
		if (next_nl == std::string::npos) {
			s_InvShellLog.push_back(str.substr(pos));
			break;
		}
		s_InvShellLog.push_back(str.substr(pos, next_nl - pos));
		pos = next_nl + 1;
	}
	s_InvScrollToBottom = true;

	std::println(stdout, "[InvShell Output]\n{}", msg);
	std::fflush(stdout);
}

// Lua internal Bridges
static int LuaBridge_Log(lua_State* L) {
	lua_Debug ar;
	std::memset(&ar, 0, sizeof(lua_Debug));

	if (lua_getstack(L, 1, &ar)) {
		lua_getinfo(L, "Sl", &ar);
	} else {
		std::strncpy(ar.short_src, "unknown", sizeof(ar.short_src) - 1);
		ar.currentline = 0;
	}

	int n = lua_gettop(L);
	std::string msg;

	lua_getglobal(L, "tostring");

	for (int i = 1; i <= n; i++) {
		lua_pushvalue(L, -1);
		lua_pushvalue(L, i);
		lua_call(L, 1, 1);

		size_t len = 0;
		const char* s = lua_tolstring(L, -1, &len);
		if (i > 1) {
			msg += "\t";
		}
		if (s) {
			msg += std::string(s, len);
		}

		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	std::string_view file = ar.short_src;
	if (auto pos = file.find_last_of("/\\"); pos != std::string_view::npos) {
		file.remove_prefix(pos + 1);
	}

	ZHLN::LogManual(file, ar.currentline, msg, ZHLN::Color::Green);
	ZHLN::GameConsole::Log(msg, {.r = 0.4f, .g = 1.0f, .b = 0.4f, .a = 1.0f});

	return 0;
}

static int LuaBridge_Warn(lua_State* L) {
	lua_Debug ar;
	std::memset(&ar, 0, sizeof(lua_Debug));

	if (lua_getstack(L, 1, &ar)) {
		lua_getinfo(L, "Sl", &ar);
	} else {
		std::strncpy(ar.short_src, "unknown", sizeof(ar.short_src) - 1);
		ar.currentline = 0;
	}

	int n = lua_gettop(L);
	std::string msg;

	lua_getglobal(L, "tostring");

	for (int i = 1; i <= n; i++) {
		lua_pushvalue(L, -1);
		lua_pushvalue(L, i);
		lua_call(L, 1, 1);

		size_t len = 0;
		const char* s = lua_tolstring(L, -1, &len);
		if (i > 1) {
			msg += "\t";
		}
		if (s) {
			msg += std::string(s, len);
		}

		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	std::string_view file = ar.short_src;
	if (auto pos = file.find_last_of("/\\"); pos != std::string_view::npos) {
		file.remove_prefix(pos + 1);
	}

	ZHLN::LogManual(file, ar.currentline, msg, ZHLN::Color::Yellow);

	return 0;
}

} // End of extern "C"

namespace ZHLN {

ScriptRunner::ScriptRunner() {
	L = luaL_newstate();
	luaL_openlibs(L);

	lua_newtable(L);
	lua_pushcfunction(L, LuaBridge_Log);
	lua_setfield(L, -2, "log");
	lua_pushcfunction(L, LuaBridge_Warn);
	lua_setfield(L, -2, "warn");
	lua_setglobal(L, "zahlen");

	lua_pushcfunction(L, LuaBridge_Log);
	lua_setglobal(L, "print");
	lua_getglobal(L, "require");
	lua_pushstring(L, "scripts.core.memoryview");
	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		Panic("Failed to load core script: scripts/core/memoryview.lua. Error: {}",
			  lua_tostring(L, -1));
	}
	lua_pop(L, 1);
}

ScriptRunner::~ScriptRunner() {
	if (L != nullptr) {
		lua_close(L);
	}
}

void ScriptRunner::RunFile(std::string_view path) {
	std::string p(path);
	if (luaL_dofile(L, p.c_str()) != LUA_OK) {
		Log("Lua Error in {}: {}", path, lua_tostring(L, -1));
		lua_pop(L, 1);
	}
}

void ScriptRunner::CallUpdate(Engine* engine, float dt) {
	lua_getglobal(L, "update");
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 1);
		return;
	}

	lua_pushlightuserdata(L, engine);
	lua_pushnumber(L, dt);

	if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
		Log("Lua Error: {}", lua_tostring(L, -1));
		lua_pop(L, 1);
	}

	lua_getglobal(L, "require");
	lua_pushstring(L, "scripts.core.zahlen");
	lua_pcall(L, 1, 1, 0);
	lua_getfield(L, -1, "cleanup");
	lua_pcall(L, 0, 0, 0);
	lua_pop(L, 1);
}

void ScriptRunner::ExecuteString(std::string_view code) {
	if (luaL_dostring(L, code.data()) != LUA_OK) {
		std::string err = lua_tostring(L, -1);
		ZHLN::GameConsole::Log("Lua Error: " + err, {1.0f, 0.4f, 0.4f, 1.0f});
		lua_pop(L, 1);
	}
}

void ScriptRunner::ReloadFile(std::string_view path) {
	std::string moduleName = std::string(path);

	if (size_t pos = moduleName.find(".lua"); pos != std::string::npos) {
		moduleName.erase(pos);
	}
	std::replace(moduleName.begin(), moduleName.end(), '/', '.');

	std::string resetCode = std::format("package.loaded['{}'] = nil", moduleName);
	luaL_dostring(L, resetCode.c_str());

	RunFile(path);

	Log("Script Hot-Reloaded: {}", path);
	ZHLN::GameConsole::Log("Hot-Reloaded: " + std::string(path),
						   {.r = 0.2f, .g = 0.8f, .b = 1.0f, .a = 1.0f});
}

} // namespace ZHLN
