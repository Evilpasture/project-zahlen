// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Thread.hpp>
#include <LLGL/Window.h>
#include <LLGL/Key.h>
// clang-format on

namespace ZHLN {

extern void JoltTraceBridge(const char* inFMT, ...) noexcept;
extern bool JoltAssertBridge(const char* inExpression, const char* inMessage, const char* inFile,
							 uint32_t inLine) noexcept;

Engine::Engine() {
	ZHLN::Fiber::InitMainThread();
	JPH::RegisterDefaultAllocator();
	JPH::Trace = JoltTraceBridge;
#ifdef JPH_ENABLE_ASSERTS
	JPH::AssertFailed = JoltAssertBridge;
#endif
#if defined(_WIN32)
	const char* preferredAPI = "Vulkan";
#elif defined(__APPLE__)
	const char* preferredAPI = "Metal";
#else
	const char* preferredAPI = "OpenGL"; 
#endif
	JPH::Factory::sInstance = new JPH::Factory();
	JPH::RegisterTypes();

	_input = std::make_unique<InputContext>();
	_window = std::make_unique<Window>("Project-Zahlen Engine", 1280, 720, _input.get());
	_renderContext = std::make_unique<RenderContext>(*_window, preferredAPI);
	_physicsContext = std::make_unique<PhysicsContext>();
}

Engine::~Engine() {
	// Destroy contexts explicitly to ensure GPU resources
	// are freed while the Jolt Factory is still alive.
	_physicsContext.reset();
	_renderContext.reset();
	_window.reset();

	JPH::UnregisterTypes();
	if (JPH::Factory::sInstance) {
		delete JPH::Factory::sInstance;
		JPH::Factory::sInstance = nullptr;
	}
}

// FIX: These now delegate to the Window class
bool Engine::IsRunning() const {
	return _window->IsRunning();
}

void Engine::ProcessEvents() {
	// 3. Clear deltas before processing new events
	_input->ResetDeltas();

	// 4. Pump the OS message queue.
	// This will internally call your OnKeyDown, OnLocalMouseMove, etc.
	LLGL::Surface::ProcessEvents();
}

void Engine::BeginFrame() {
	_renderContext->BeginFrame();
}

void Engine::EndFrame() {
	_renderContext->EndFrame();
}

} // namespace ZHLN