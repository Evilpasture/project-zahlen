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
#include <threading/Thread.hpp>

namespace ZHLN {
thread_local Engine* g_CurrentEngine = nullptr;
extern void JoltTraceBridge(const char* inFMT, ...) noexcept;
extern bool JoltAssertBridge(const char* inExpression, const char* inMessage, const char* inFile,
							 uint32_t inLine) noexcept;

Engine::Engine(const EngineConfig& cfg) {
	g_CurrentEngine = this;
	ZHLN::Fiber::InitMainThread();

	JPH::RegisterDefaultAllocator();
	JPH::Trace = JoltTraceBridge;
#ifdef JPH_ENABLE_ASSERTS
	JPH::AssertFailed = JoltAssertBridge;
#endif

	JPH::Factory::sInstance = new JPH::Factory();
	JPH::RegisterTypes();

	// 1. Initialize GLFW here
	if (!glfwInit()) {
		ZHLN::Panic("FATAL: Failed to initialize GLFW");
	}

	_input = std::make_unique<InputContext>();
	_window = std::make_unique<Window>("Project-Zahlen Engine", cfg.render.width, cfg.render.height,
									   _input.get());

	// We just pass Vulkan now. The renderer internally handles the rest.
	_renderContext = std::make_unique<RenderContext>(*_window);

	_physicsContext = std::make_unique<PhysicsContext>(cfg.physics);
}

Engine::~Engine() {
	_physicsContext.reset();
	_renderContext.reset();
	_window.reset();

	// 2. Terminate GLFW
	glfwTerminate();

	JPH::UnregisterTypes();
	if (JPH::Factory::sInstance) {
		delete JPH::Factory::sInstance;
		JPH::Factory::sInstance = nullptr;
	}
}

bool Engine::IsRunning() const {
	return _window->IsRunning();
}

void Engine::ProcessEvents() {
	_input->ResetDeltas();
	glfwPollEvents();

	// Start the ImGui Frame immediately after polling events
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();
}

void Engine::BeginFrame() {
	// This now only handles the GPU side
	_renderContext->BeginFrame();
}

void Engine::EndFrame() {
	_renderContext->EndFrame();
}

} // namespace ZHLN