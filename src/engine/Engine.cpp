// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/Engine.cpp

#include <GLFW/glfw3.h>
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
// clang-format on
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
#include <ecs/ECS.hpp>
#include <filesystem>
#include <physics/Physics.hpp>
#include <threading/Thread.hpp>

namespace ZHLN {

thread_local Engine* g_CurrentEngine = nullptr;
static Engine* s_GlobalEngine = nullptr;

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
};

Engine::Engine() : Engine(EngineConfig{}) {}

Engine::Engine(const EngineConfig& cfg) {
	bool success = false;
	InitInternal(cfg, success);
	if (!success) {
		ZHLN::Panic("FATAL: Failed to initialize Engine via legacy constructor.");
	}
}

Engine::Engine(const EngineConfig& cfg, bool& outSuccess) {
	InitInternal(cfg, outSuccess);
}

std::unique_ptr<Engine> Engine::Create(const EngineConfig& cfg, const char** outError) {
	bool success = false;
	auto engine = std::unique_ptr<Engine>(new (std::nothrow) Engine(cfg, success));

	if (!engine || !success) {
		if (outError != nullptr) {
			*outError = "Failed to initialize windowing system (GLFW). Are you running in a "
						"headless, SSH, or TTY environment without an active X11/Wayland session?";
		}
		return nullptr;
	}
	return engine;
}

void Engine::InitInternal(const EngineConfig& cfg, bool& outSuccess) {
	outSuccess = false;
	g_CurrentEngine = this;
	s_GlobalEngine = this;
	ZHLN::Fiber::InitMainThread();

	JPH::RegisterDefaultAllocator();
	JPH::Trace = JoltTraceBridge;
#ifdef JPH_ENABLE_ASSERTS
	JPH::AssertFailed = JoltAssertBridge;
#endif

	JPH::Factory::sInstance = new JPH::Factory();
	JPH::RegisterTypes();

	if (!glfwInit()) {
		return; // Gracefully return; outSuccess remains false
	}

	_impl = std::make_unique<EngineImpl>();

	_impl->input = std::make_unique<InputContext>();
	_impl->window =
		std::make_unique<Window>(cfg.render.appName.data(), cfg.render.width, cfg.render.height,
								 cfg.render.fullscreen, _impl->input.get());

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
