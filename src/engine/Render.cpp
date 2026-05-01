#include "engine/Render.hpp"

#include "engine/Log.hpp"

namespace ZHLN {

Renderer::Renderer(const String32& preferredAPI, uint32_t width, uint32_t height) {
	// 1. Pick the API
	auto modules = LLGL::RenderSystem::FindModules();
	String32 selected = "OpenGL";
	for (const auto& m : modules) {
		if (m == preferredAPI.c_str()) {
			selected = m.c_str();
			break;
		}
	}

	ZHLN::Log("Loading Renderer: {}\n", selected);

	// 2. Load System (Unique Ownership)
	_system = LLGL::RenderSystem::Load(LLGL::RenderSystemDescriptor{selected.c_str()});
	if (!_system)
		return;

	// 3. Create Window (Shared because SwapChain needs it)
	_window = LLGL::Window::Create(LLGL::WindowDescriptor{
		.title = "Project-Zahlen",
		.position = {}, // Must be explicitly initialized because compiler says so
		.size = {width, height},
		.flags = LLGL::WindowFlags::Visible | LLGL::WindowFlags::Resizable |
				 LLGL::WindowFlags::Centered});

	// 4. Create SwapChain
	// We pass the _window (shared_ptr) here
	_swapChain = _system->CreateSwapChain(
		LLGL::SwapChainDescriptor{.resolution = _window->GetSize(), .samples = 8}, _window);

	// 5. Get Command interfaces (Managed by System, so we use raw pointers)
	_commandQueue = _system->GetCommandQueue();

	// Command Buffer is a unique object we create
	_cmdBuffer = std::unique_ptr<LLGL::CommandBuffer>(_system->CreateCommandBuffer());
}

Renderer::~Renderer() {
	if (_system) {
		// Because LLGL::RenderSystemPtr has a custom deleter,
		// we must use Unload to trigger the final cleanup correctly.
		LLGL::RenderSystem::Unload(std::move(_system));
	}
}

bool Renderer::IsRunning() const {
	return _window && !_window->HasQuit();
}

void Renderer::ProcessEvents() {
	_window->ProcessEvents();
}

void Renderer::BeginFrame() {
	_cmdBuffer->Begin();
	_cmdBuffer->BeginRenderPass(*_swapChain);
}

void Renderer::Clear(const JPH::Vec4& color) {
	LLGL::ClearValue val = {color.GetX(), color.GetY(), color.GetZ(), color.GetW()};
	_cmdBuffer->Clear(LLGL::ClearFlags::Color, val);
}

void Renderer::EndFrame() {
	_cmdBuffer->EndRenderPass();
	_cmdBuffer->End();
	_commandQueue->Submit(*_cmdBuffer);
	_swapChain->Present();
}

} // namespace ZHLN