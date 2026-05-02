#include "engine/Render.hpp"

#include "engine/Log.hpp"

#include <utility>

namespace ZHLN {

// --- RenderContext Implementation ---

RenderContext::RenderContext(const String32& preferredAPI, uint32_t width, uint32_t height) {
	auto modules = LLGL::RenderSystem::FindModules();
	String32 selected = "OpenGL";
	for (const auto& m : modules) {
		if (m == preferredAPI.c_str()) {
			selected = m.c_str();
			break;
		}
	}

	ZHLN::Log("Loading RenderContext: {}\n", selected);

	_system = LLGL::RenderSystem::Load(LLGL::RenderSystemDescriptor{selected.c_str()});
	if (!_system)
		return;

	_window = LLGL::Window::Create(
		LLGL::WindowDescriptor{.title = "Project-Zahlen Engine",
							   .position = {},
							   .size = {width, height},
							   .flags = LLGL::WindowFlags::Visible | LLGL::WindowFlags::Centered});

	_swapChain =
		_system->CreateSwapChain({.resolution = _window->GetSize(), .samples = 8}, _window);
	_commandQueue = _system->GetCommandQueue();
	_cmdBuffer = std::unique_ptr<LLGL::CommandBuffer>(_system->CreateCommandBuffer());
}

RenderContext::~RenderContext() {
	if (_system) {
		LLGL::RenderSystem::Unload(std::move(_system));
	}
}

bool RenderContext::IsRunning() const {
	return _window && !_window->HasQuit();
}

void RenderContext::ProcessEvents() {
	_window->ProcessEvents();
}

void RenderContext::BeginFrame() {
	_cmdBuffer->Begin();
	_cmdBuffer->BeginRenderPass(*_swapChain);
}

void RenderContext::EndFrame() {
	_cmdBuffer->EndRenderPass();
	_cmdBuffer->End();
	_commandQueue->Submit(*_cmdBuffer);
	_swapChain->Present();
}

// --- Procedural Renderer Service ---

namespace Renderer {

void Clear(RenderContext& ctx, const JPH::Vec4& color) {
	LLGL::ClearValue val;
	using Ch = ColorComponent;
	val.color[std::to_underlying(Ch::R)] = color.GetX();
	val.color[std::to_underlying(Ch::G)] = color.GetY();
	val.color[std::to_underlying(Ch::B)] = color.GetZ();
	val.color[std::to_underlying(Ch::A)] = color.GetW();
	ctx.GetCommandBuffer()->Clear(LLGL::ClearFlags::Color, val);
}

void UpdateBuffer(RenderContext& ctx, LLGL::Buffer* buffer, const void* data, size_t size) {
	ctx.GetSystem()->WriteBuffer(*buffer, 0, data, size);
}

void Draw(RenderContext& ctx, LLGL::PipelineState* pipeline, LLGL::ResourceHeap* resources,
		  LLGL::Buffer* vertexBuffer, uint32_t vertexCount) {
	auto* cmd = ctx.GetCommandBuffer();
	cmd->SetVertexBuffer(*vertexBuffer);
	cmd->SetPipelineState(*pipeline);

	if (resources) {
		cmd->SetResourceHeap(*resources);
	}

	cmd->Draw(vertexCount, 0);
}

} // namespace Renderer
} // namespace ZHLN