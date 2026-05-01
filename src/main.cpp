// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Math/Mat44.h> // Needed for Mat44::sRotationZ

#include "engine/Render.hpp"
#include "engine/Log.hpp"
// clang-format on

using namespace ZHLN;

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
	JPH::RegisterDefaultAllocator();
	JPH::Trace = ZHLN::JoltTraceBridge;
#ifdef JPH_ENABLE_ASSERTS
	JPH::AssertFailed = ZHLN::JoltAssertBridge;
#endif
	JPH::Factory::sInstance = new JPH::Factory();
	JPH::RegisterTypes();

	{
		Renderer renderer("Metal", 1280, 720);
		const JPH::Vec4 background(0.12f, 0.14f, 0.16f, 1.0f);

		float rotation = 0.0f;

		while (renderer.IsRunning()) {
			renderer.ProcessEvents();

			// Generate a new transformation matrix every frame
			rotation += 0.05f; // Adjust speed here
			JPH::Mat44 transform = JPH::Mat44::sRotationZ(rotation);

			renderer.BeginFrame();
			renderer.Clear(background);

			// Pass the matrix to the GPU
			renderer.DrawTriangle(transform);

			renderer.EndFrame();
		}
	}

	JPH::UnregisterTypes();
	delete JPH::Factory::sInstance;
	return 0;
}