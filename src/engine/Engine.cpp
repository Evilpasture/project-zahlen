// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/Engine.cpp

#include <GLFW/glfw3.h>
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
// clang-format on
#include "TTYBackend.hpp"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"

#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/alife/Simulator.hpp>
#include <renderdoc_app.h>
#ifdef __linux__
#include <dlfcn.h>
#endif
#include <ecs/ECS.hpp>
#include <filesystem>
#include <physics/Physics.hpp>
#include <threading/Thread.hpp>

namespace ZHLN {

thread_local Engine* g_CurrentEngine = nullptr;
static Engine* s_GlobalEngine = nullptr;
static RENDERDOC_API_1_5_0* s_RDocAPI = nullptr;

static void InitRenderDocAPI() {
#if defined(_WIN32)
	if (HMODULE mod = GetModuleHandleA("renderdoc.dll")) {
		pRENDERDOC_GetAPI R_GetAPI = (pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI");
		if (R_GetAPI) {
			R_GetAPI(eRENDERDOC_API_Version_1_5_0, (void**)&s_RDocAPI);
		}
	}
#elif defined(__linux__)
	if (void* mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD)) {
		auto R_GetAPI = (pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
		if (R_GetAPI != nullptr) {
			R_GetAPI(eRENDERDOC_API_Version_1_5_0, (void**)&s_RDocAPI);
		}
	}
#endif
	if (s_RDocAPI != nullptr) {
		ZHLN::Log("[RenderDoc] In-App API successfully bound.");
	}
}

struct EngineImpl {
	std::unique_ptr<InputContext> input;
	std::unique_ptr<Window> window;
	std::unique_ptr<RenderContext> renderContext;
	std::unique_ptr<PhysicsContext> physicsContext;
	std::unique_ptr<AudioContext> audioContext;
	std::unique_ptr<ALife::Simulator> alifeSimulator;
	std::unique_ptr<AssetManager> assetManager;

	Camera mainCamera;
	ECS::Registry registry;

	void* gameState = nullptr;
	uint64_t frameCounter = 0;
};

Engine::Engine() : _impl(nullptr) {
	// Empty constructor specifically used by static factory Create() to defer initialization safely
}

Engine::Engine(const EngineConfig& cfg) : _impl(nullptr) {
	bool success = false;
	InitInternal(cfg, success, nullptr);
	if (!success) {
		ZHLN::Panic("FATAL: Failed to initialize Engine via legacy constructor.");
	}
}

Engine::Engine(const EngineConfig& cfg, bool& outSuccess) : _impl(nullptr) {
	InitInternal(cfg, outSuccess, nullptr);
}

std::unique_ptr<Engine> Engine::Create(const EngineConfig& cfg, const char** outError) {
	auto engine = std::unique_ptr<Engine>(new (std::nothrow) Engine());
	if (!engine) {
		if (outError != nullptr) {
			*outError = "Failed to allocate memory for the Engine context.";
		}
		return nullptr;
	}

	bool success = false;
	engine->InitInternal(cfg, success, outError);

	if (!success) {
		return nullptr; // outError has been populated with the precise failure reason
	}

	return engine;
}

void Engine::InitInternal(const EngineConfig& cfg, bool& outSuccess, const char** outError) {
	outSuccess = false;
	g_CurrentEngine = this;
	s_GlobalEngine = this;

	// 1. Thread and Fiber setup
	ZHLN::Fiber::InitMainThread();

	// 2. Allocate the implementation block and input context first
	_impl = std::make_unique<EngineImpl>();
	_impl->input = std::make_unique<InputContext>();

	// 3. Apply platform-specific window managers / RenderDoc hints
	bool use_tty = false;
	if constexpr (isLinux) {
		// If RenderDoc is active, force GLFW to initialize on X11 (XWayland)
		// to match RenderDoc's active WSI extensions.
		if (std::getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE") != nullptr) {
			glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
		}
	}

	// 4. Initialize GLFW
	if (!glfwInit()) {
		if (TTYBackend::IsSupported()) {
			ZHLN::Log("GLFW failed to initialize. Falling back to native TTY Display Mode.");
			use_tty = true;
		} else {
			if (outError != nullptr) {
				*outError = "GLFW failed to initialize, and native KMS/TTY display mode is not "
							"supported on this platform.";
			}
			return;
		}
	}

	// 5. Create and show the Window immediately
	_impl->window =
		std::make_unique<Window>(cfg.render.appName.data(), cfg.render.width, cfg.render.height,
								 cfg.render.fullscreen, _impl->input.get(), use_tty);

	// Direct TTY Check (fails cleanly if libseat is missing or seatd is not running)
	if (use_tty && _impl->window->GetTTYContext() == nullptr) {
		ZHLN::Log("[Engine] FATAL: TTY Input initialization failed (libseat session rejected).");
		if (outError != nullptr) {
			*outError = "TTY input initialization failed. Please make sure seatd.service is active "
						"or logind is running.";
		}
		return;
	}

	// 6. Query the RenderDoc In-App API (if loaded by command line flag)
	InitRenderDocAPI();

	// 7. Initialize Jolt Physics global allocations
	JPH::RegisterDefaultAllocator();
	JPH::Trace = JoltTraceBridge;
#ifdef JPH_ENABLE_ASSERTS
	JPH::AssertFailed = JoltAssertBridge;
#endif

	JPH::Factory::sInstance = new JPH::Factory();
	JPH::RegisterTypes();

	// 8. Initialize remaining graphics, physics, and asset loader systems
	_impl->renderContext = std::make_unique<RenderContext>(*_impl->window, cfg.render);
	_impl->physicsContext = std::make_unique<PhysicsContext>(cfg.physics);
	_impl->audioContext = std::make_unique<AudioContext>();
	_impl->alifeSimulator = std::make_unique<ALife::Simulator>();
	_impl->assetManager = std::make_unique<AssetManager>();

	if (std::filesystem::exists("data/base.pak")) {
		_impl->assetManager->MountPak("data/base.pak");
	} else if (std::filesystem::exists("build/data/base.pak")) {
		_impl->assetManager->MountPak("build/data/base.pak");
	} else {
		ZHLN::Log("WARNING: Could not find 'data/base.pak' in working directory or build/ folder!");
	}

	outSuccess = true;
}

Engine::~Engine() {
	_impl->physicsContext.reset();
	_impl->renderContext.reset();
	_impl->window.reset();
	_impl->assetManager.reset();
	_impl->audioContext.reset();

	glfwTerminate();

	JPH::UnregisterTypes();
	if (JPH::Factory::sInstance != nullptr) {
		delete JPH::Factory::sInstance;
		JPH::Factory::sInstance = nullptr;
	}
}

bool Engine::IsRunning() const {
	return _impl->window->IsRunning();
}

void Engine::ProcessEvents() {
	ZHLN::CheckForCrashes(this);
	_impl->input->ResetDeltas();

	if (_impl->window->IsTTY()) {
		// Process raw evdev keyboard and mouse inputs directly
		TTYBackend::ProcessEvents(_impl->window->GetTTYContext(), _impl->input.get());

		// No ImGui contexts exist, so we exit immediately (prevents the 0x40 Null Pointer crash!)
		return;
	}

	// Desktop GLFW + ImGui path
	glfwPollEvents();
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();
}

void Engine::BeginFrame() {
	_impl->renderContext->BeginFrame();
}

void Engine::EndFrame() {
	_impl->renderContext->EndFrame();
}

uint64_t Engine::GetCurrentFrame() const noexcept {
	return _impl->frameCounter;
}

Window& Engine::GetWindow() {
	return *_impl->window;
}
PhysicsContext& Engine::GetPhysicsContext() {
	return *_impl->physicsContext;
}
RenderContext& Engine::GetRenderContext() {
	return *_impl->renderContext;
}
InputContext& Engine::GetInput() {
	return *_impl->input;
}
Camera& Engine::GetCamera() {
	return _impl->mainCamera;
}
ALife::Simulator& Engine::GetALife() {
	return *_impl->alifeSimulator;
}
AssetManager& Engine::GetAssetManager() {
	return *_impl->assetManager;
}
AudioContext& Engine::GetAudioContext() {
	return *_impl->audioContext;
}
ECS::Registry& Engine::GetRegistry() {
	return _impl->registry;
}

void* Engine::GetGameState() const {
	return _impl->gameState;
}
void Engine::SetGameState(void* state) {
	_impl->gameState = state;
}

Engine* GetEngineContext() {
	if (g_CurrentEngine != nullptr) {
		return g_CurrentEngine;
	}
	return s_GlobalEngine;
}

} // namespace ZHLN
