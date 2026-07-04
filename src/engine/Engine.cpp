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

#include <Zahlen/CreativeWorksManager.hpp>
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
#include <ecs/EntityCommandBuffer.hpp>
#include <ecs/SystemGraph.hpp>
#include <engine/system/CullingSystem.hpp>
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

namespace CreativeWorksFactory {
void RebuildVulkanResources(RenderContext& ctx, CreativeWorksManager& assetMgr, ECS::Registry& reg);
}

struct EngineImpl {
	std::unique_ptr<InputContext> input;
	std::unique_ptr<Window> window;
	std::unique_ptr<RenderContext> renderContext;
	std::unique_ptr<PhysicsContext> physicsContext;
	std::unique_ptr<AudioContext> audioContext;
	std::unique_ptr<ALife::Simulator> alifeSimulator;
	std::unique_ptr<CreativeWorksManager> assetManager;

	Camera mainCamera;
	ECS::Registry registry;

	std::unique_ptr<ECS::SystemGraph> updateGraph;
	std::unique_ptr<ECS::SystemGraph> renderGraph;
	std::unique_ptr<ECS::EntityCommandBuffer> mainECB;
	std::unique_ptr<CullingSystem> cullingSystem;
	JPH::Array<Entity> visibleEntities;
	JPH::Array<Entity> visibleShadowEntities;
	float currentAlpha = 0.0f;

	void* gameState = nullptr;
	uint64_t frameCounter = 0;
	EngineConfig config;
};

Engine::Engine() : _impl(nullptr) {}

Engine::Engine(const EngineConfig& cfg) : _impl(nullptr) {
	bool success = false;
	InitInternal(cfg, success, nullptr);
	if (!success) {
		ZHLN::Panic("FATAL: Failed to initialize Engine via legacy constructor.");
	}
}

void Engine::HandleDeviceLost() noexcept {
	ZHLN::Log("[Engine] CRITICAL: Vulkan Device Lost detected! Initiating hardware hot-rebuild...");

	// 1. Reset and Recreate Render Context (New Vulkan Device)
	_impl->renderContext.reset();
	_impl->renderContext = std::make_unique<RenderContext>(*_impl->window, _impl->config.render);

	// 2. Perform True Rebinding Recovery (Zero physics/registry/scripting resets)
	CreativeWorksFactory::RebuildVulkanResources(*_impl->renderContext, *_impl->assetManager,
										 _impl->registry);

	ZHLN::Log("[Engine] Hardware hot-rebuild completed successfully. All visual assets rebound to "
			  "new GPU.");
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

	ZHLN::Fiber::InitMainThread();

	_impl = std::make_unique<EngineImpl>();
	_impl->config = cfg;
	_impl->input = std::make_unique<InputContext>();

	bool use_tty = false;
	if constexpr (isLinux) {
		// Detects both RenderDoc and NVIDIA Nsight Graphics (Nomad) launch environments
		if (std::getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE") != nullptr ||
			std::getenv("NOMAD_VULKAN_LAYER") != nullptr ||
			std::getenv("NGFX_INJECTION") != nullptr) {
			glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
		}
	}

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

	_impl->window =
		std::make_unique<Window>(cfg.render.appName.data(), cfg.render.width, cfg.render.height,
								 cfg.render.fullscreen, _impl->input.get(), use_tty);

	if (use_tty && _impl->window->GetTTYContext() == nullptr) {
		ZHLN::Log("[Engine] FATAL: TTY Input initialization failed (libseat session rejected).");
		if (outError != nullptr) {
			*outError = "TTY input initialization failed. Please make sure seatd.service is active "
						"or logind is running.";
		}
		return;
	}

	InitRenderDocAPI();

	JPH::RegisterDefaultAllocator();
	JPH::Trace = JoltTraceBridge;
#ifdef JPH_ENABLE_ASSERTS
	JPH::AssertFailed = JoltAssertBridge;
#endif

	JPH::Factory::sInstance = new JPH::Factory();
	JPH::RegisterTypes();

	_impl->renderContext = std::make_unique<RenderContext>(*_impl->window, cfg.render);
	_impl->physicsContext = std::make_unique<PhysicsContext>(cfg.physics);
	_impl->audioContext = std::make_unique<AudioContext>();
	_impl->alifeSimulator = std::make_unique<ALife::Simulator>();
	_impl->assetManager = std::make_unique<CreativeWorksManager>();

	_impl->updateGraph = std::make_unique<ECS::SystemGraph>();
	_impl->renderGraph = std::make_unique<ECS::SystemGraph>();
	_impl->mainECB = std::make_unique<ECS::EntityCommandBuffer>(_impl->registry);
	_impl->cullingSystem = std::make_unique<CullingSystem>();

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
	_impl->registry.Clear();
	_impl->physicsContext.reset();
	_impl->renderContext.reset();
	_impl->window.reset();
	_impl->assetManager.reset();
	_impl->audioContext.reset();
	_impl->updateGraph.reset();
	_impl->renderGraph.reset();
	_impl->mainECB.reset();
	_impl->cullingSystem.reset();

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
		TTYBackend::ProcessEvents(_impl->window->GetTTYContext(), _impl->input.get());
		return;
	}

	glfwPollEvents();
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();
}

bool Engine::BeginFrame(bool& outDeviceLost) noexcept {
	outDeviceLost = false;
	auto res = _impl->renderContext->BeginFrame();
	if (!res) {
		if (res.error() == RenderFrameResult::DeviceLost) {
			outDeviceLost = true;
			HandleDeviceLost();
		}
		return false;
	}
	return true;
}

bool Engine::EndFrame(bool& outDeviceLost) noexcept {
	outDeviceLost = false;
	auto res = _impl->renderContext->EndFrame();
	if (!res) {
		if (res.error() == RenderFrameResult::DeviceLost) {
			outDeviceLost = true;
			HandleDeviceLost();
		}
		return false;
	}
	return true;
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
CreativeWorksManager& Engine::GetCreativeWorksManager() {
	return *_impl->assetManager;
}
AudioContext& Engine::GetAudioContext() {
	return *_impl->audioContext;
}
ECS::Registry& Engine::GetRegistry() {
	return _impl->registry;
}

ECS::SystemGraph& Engine::GetUpdateGraph() {
	return *_impl->updateGraph;
}
ECS::SystemGraph& Engine::GetRenderGraph() {
	return *_impl->renderGraph;
}
ECS::EntityCommandBuffer& Engine::GetMainECB() {
	return *_impl->mainECB;
}
CullingSystem& Engine::GetCullingSystem() {
	return *_impl->cullingSystem;
}
JPH::Array<Entity>& Engine::GetVisibleEntities() {
	return _impl->visibleEntities;
}
JPH::Array<Entity>& Engine::GetVisibleShadowEntities() {
	return _impl->visibleShadowEntities;
}
float& Engine::GetCurrentAlpha() {
	return _impl->currentAlpha;
}

void* Engine::GetGameState() const {
	return _impl->gameState;
}
void Engine::SetGameState(void* state) {
	_impl->gameState = state;
}

void Engine::ProvokeDeviceLost() {
	_impl->renderContext->ProvokeDeviceLost();
}

Engine* GetEngineContext() {
	if (g_CurrentEngine != nullptr) {
		return g_CurrentEngine;
	}
	return s_GlobalEngine;
}

} // namespace ZHLN
