#include <Zahlen/Render.hpp>

#include <Zahlen/Log.hpp>

#include <utility>

namespace ZHLN {

RenderContext::RenderContext(Window& window, const String32& preferredAPI) {
    auto modules = LLGL::RenderSystem::FindModules();
    String32 selected = "";

    // 1. Try to find the one we actually want
    for (const auto& m : modules) {
        if (m == preferredAPI.c_str()) {
            selected = m.c_str();
            break;
        }
    }

    // 2. Fallback: If preferred is missing, pick the first available one
    if (selected.empty() && !modules.empty()) {
        selected = modules[0].c_str();
        ZHLN::Log("WARNING: Preferred API '{}' not found. Falling back to '{}'\n", 
                  preferredAPI.c_str(), selected.c_str());
    }

    if (selected.empty()) {
        ZHLN::Log("FATAL: No RenderSystem modules found!\n");
        return;
    }

    ZHLN::Log("Loading RenderContext: {}\n", selected.c_str());
    _system = LLGL::RenderSystem::Load(LLGL::RenderSystemDescriptor{selected.c_str()});
	if (!_system)
		return;

	// The SwapChain connects the RenderSystem to the Window's Native Surface
	LLGL::SwapChainDescriptor swapChainDesc;
	swapChainDesc.resolution = window.GetSize();
	swapChainDesc.depthBits = 32;
	swapChainDesc.samples = 8;

	_swapChain = _system->CreateSwapChain(swapChainDesc, window.GetNative());
	_commandQueue = _system->GetCommandQueue();
	_cmdBuffer = LLGLPtr<LLGL::CommandBuffer>(
        _system->CreateCommandBuffer(), 
        LLGLDeleter{_system.get()} 
    );
}

RenderContext::~RenderContext() {
    if (_system) {
        // 1. Manually kill the command buffer first while the system is alive
        _cmdBuffer.reset();
        
        // 2. The SwapChain is owned by the system, so we don't reset it, 
        // but we ensure no one else is using it.
        
        // 3. Now it is safe to unload the system
        LLGL::RenderSystem::Unload(std::move(_system));
    }
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

void RenderContext::SetResolution(const LLGL::Extent2D& resolution) {
    // Vulkan cannot create a swapchain with 0 width or height (minimization)
    if (resolution.width == 0 || resolution.height == 0) {
        return;
    }

    if (_swapChain) {
        _swapChain->ResizeBuffers(resolution);
        ZHLN::Log("Vulkan Swapchain resized to {}x{}\n", resolution.width, resolution.height);
    }
}

namespace Renderer {

void Clear(RenderContext& ctx, const JPH::Vec4& color, float depth) {
	LLGL::ClearValue val;
	using Ch = ColorComponent;
	val.color[std::to_underlying(Ch::R)] = color.GetX();
	val.color[std::to_underlying(Ch::G)] = color.GetY();
	val.color[std::to_underlying(Ch::B)] = color.GetZ();
	val.color[std::to_underlying(Ch::A)] = color.GetW();
	val.depth = depth;

	ctx.GetCommandBuffer()->Clear(LLGL::ClearFlags::Color | LLGL::ClearFlags::Depth, val);
}

void UpdateBuffer(RenderContext& ctx, LLGL::Buffer* buffer, const void* data, size_t size) {
	ctx.GetSystem()->WriteBuffer(*buffer, 0, data, size);
}

void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh,
		  const JPH::Mat44& transform) {
	UpdateBuffer(ctx, material.constantBuffer.get(), transform);

	auto* cmd = ctx.GetCommandBuffer();
	cmd->SetPipelineState(*material.pipeline);
	cmd->SetResourceHeap(*material.resourceHeap);
	cmd->SetVertexBuffer(*mesh.vertexBuffer);
	cmd->Draw(mesh.vertexCount, 0);
}

} // namespace Renderer
} // namespace ZHLN