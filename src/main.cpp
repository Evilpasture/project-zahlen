// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

#include "engine/Render.hpp"
#include "engine/Log.hpp"
// clang-format on

using namespace ZHLN;

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
	// Initialize Jolt
	JPH::RegisterDefaultAllocator();
	JPH::Trace = ZHLN::JoltTraceBridge;
#ifdef JPH_ENABLE_ASSERTS
	JPH::AssertFailed = ZHLN::JoltAssertBridge;
#endif
	JPH::Factory::sInstance = new JPH::Factory();
	JPH::RegisterTypes();

	{
		// Native renderer for macOS is "Metal"
		Renderer renderer("Metal", 1280, 720);

		// Jolt Math clear color (Midnight Gray)
		const JPH::Vec4 background(0.12f, 0.14f, 0.16f, 1.0f);

		while (renderer.IsRunning()) {
			renderer.ProcessEvents();

			renderer.BeginFrame();
			renderer.Clear(background);
			renderer.EndFrame();
		}
	}

	// Manual Cleanup
	JPH::UnregisterTypes();
	delete JPH::Factory::sInstance;

	return 0;
}