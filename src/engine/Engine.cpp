// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include "engine/Engine.hpp"
#include "engine/Log.hpp"
// clang-format on

namespace ZHLN {

extern void JoltTraceBridge(const char* inFMT, ...) noexcept;
extern bool JoltAssertBridge(const char* inExpression, const char* inMessage, const char* inFile,
							 uint32_t inLine) noexcept;

Engine::Engine() {
	// 1. Initialize Physics Globals
	JPH::RegisterDefaultAllocator();
	JPH::Trace = JoltTraceBridge;
#ifdef JPH_ENABLE_ASSERTS
	JPH::AssertFailed = JoltAssertBridge;
#endif
	JPH::Factory::sInstance = new JPH::Factory();
	JPH::RegisterTypes();

	// 2. Initialize OS Window
	_window = std::make_unique<Window>("Project-Zahlen Engine", 1280, 720);

	// 3. Initialize Graphics (Now takes the window reference)
	_renderContext = std::make_unique<RenderContext>(*_window, "Metal");

	// 4. Initialize Physics System
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
	_window->ProcessEvents();
}

void Engine::BeginFrame() {
	_renderContext->BeginFrame();
}

void Engine::EndFrame() {
	_renderContext->EndFrame();
}

} // namespace ZHLN