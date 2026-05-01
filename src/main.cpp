// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>      // Required for JPH::Factory
#include <Jolt/Core/Memory.h>       // Required for RegisterDefaultAllocator
#include <Jolt/Core/Array.h>        // Required for JPH::Array
#include <Jolt/RegisterTypes.h>     // Required for RegisterTypes

#include "engine/detail/Loop.hpp"
#include "engine/detail/Prefetch.hpp"
#include "engine/detail/Span.hpp"
#include "engine/detail/String.hpp"
#include "engine/Log.hpp"

#include <LLGL/LLGL.h>
#include <print>
#include <cstdarg>
// clang-format on

using namespace ZHLN;

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
	// 1. Initialize Jolt Global Systems
	JPH::RegisterDefaultAllocator();

	// Hook Jolt's callback pointers
	JPH::Trace = ZHLN::JoltTraceBridge;
#ifdef JPH_ENABLE_ASSERTS
	JPH::AssertFailed = ZHLN::JoltAssertBridge;
#endif

	// Create the factory and register physics types
	JPH::Factory::sInstance = new JPH::Factory();
	JPH::RegisterTypes();

	// 2. Determine Renderer Module
	auto availableModules = LLGL::RenderSystem::FindModules();

	// Use JPH::Array to store our FixedStrings
	JPH::Array<String32> modules;
	for (const auto& m : availableModules) {
		modules.push_back(String32(m.c_str()));
	}

	String32 rendererModule = "OpenGL"; // Default
	for (const auto& m : modules) {
		// Prioritize modern APIs (Metal for your macOS environment)
		if (m == "Metal" || m == "Vulkan" || m == "Direct3D12") {
			rendererModule = m;
			break;
		}
	}

	std::println("Project-Zahlen initializing with {}...", rendererModule);

	// 3. Setup Graphics
	LLGL::RenderSystemDescriptor rendererDesc(rendererModule.c_str());
	auto renderSystem = LLGL::RenderSystem::Load(rendererDesc);
	if (!renderSystem) {
		std::println("Critical: Failed to load renderer.");
		return 1;
	}

	String128 title = "Zahlen Engine [";
	title.append(rendererModule);
	title.append("]");

	std::shared_ptr<LLGL::Window> window = LLGL::Window::Create(LLGL::WindowDescriptor{
		.title = title.c_str(),
		.position = {},
		.size = {1280, 720},
		.flags =
			LLGL::WindowFlags::Visible | LLGL::WindowFlags::Resizable | LLGL::WindowFlags::Centered,
	});

	auto* swapChain = renderSystem->CreateSwapChain(
		LLGL::SwapChainDescriptor{
			.resolution = window->GetSize(),
			.samples = 8,
		},
		window);

	auto* commandQueue = renderSystem->GetCommandQueue();
	auto* commandBuffer = renderSystem->CreateCommandBuffer();

	// 4. Main Loop
	while (window->ProcessEvents() && !window->HasQuit()) {
		commandBuffer->Begin();
		{
			commandBuffer->BeginRenderPass(*swapChain);
			{
				commandBuffer->Clear(LLGL::ClearFlags::Color, {0.1f, 0.12f, 0.15f, 1.0f});
			}
			commandBuffer->EndRenderPass();
		}
		commandBuffer->End();

		commandQueue->Submit(*commandBuffer);
		swapChain->Present();
	}

	// 5. Shutdown
	LLGL::RenderSystem::Unload(std::move(renderSystem));

	// Cleanup Jolt
	JPH::UnregisterTypes();
	delete JPH::Factory::sInstance;
	JPH::Factory::sInstance = nullptr;

	return 0;
}