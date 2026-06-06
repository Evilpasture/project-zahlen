#include <GLFW/glfw3.h>
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
// clang-format on
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"

#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <filesystem>
#include <threading/Thread.hpp>

// Complete subsystem definitions required only inside this translation unit
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/alife/Simulator.hpp>
#include <ecs/ECS.hpp>
#include <physics/Physics.hpp>

namespace ZHLN {

thread_local Engine* g_CurrentEngine = nullptr;
static Engine* s_GlobalEngine = nullptr;

// Implementation block containing the actual subsystem instances
struct EngineImpl {
	std::unique_ptr<InputContext> input;
	std::unique_ptr<Window> window;
	std::unique_ptr<RenderContext> renderContext;
	std::unique_ptr<PhysicsContext> physicsContext;
	std::unique_ptr<AudioContext> audioContext;
	std::unique_ptr<ALife::Simulator> alifeSimulator;
	std::unique_ptr<AssetManager> assetManager;

	// Kept as concrete stack-allocated members to avoid double-indirection
	Camera mainCamera;
	ECS::Registry registry;
};

Engine::Engine() : Engine(EngineConfig{}) {}

Engine::Engine(const EngineConfig& cfg) {
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
		ZHLN::Panic("FATAL: Failed to initialize GLFW");
	}

	_impl = std::make_unique<EngineImpl>();

	_impl->input = std::make_unique<InputContext>();
	_impl->window = std::make_unique<Window>("Project-Zahlen Engine", cfg.render.width,
											 cfg.render.height, _impl->input.get());

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

// Facade dispatch mapping
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

Engine* GetEngineContext() {
	if (g_CurrentEngine != nullptr) {
		return g_CurrentEngine;
	}
	return s_GlobalEngine;
}

} // namespace ZHLN
