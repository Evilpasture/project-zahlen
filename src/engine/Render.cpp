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
    // Query the window's resolution—LLGL's window->GetSize() usually 
    // returns logical units. Check if LLGL has a physical size query.
    swapChainDesc.resolution = window.GetNative()->GetSize(); 
    swapChainDesc.depthBits  = 32;
    swapChainDesc.samples    = 8;
    swapChainDesc.resizable  = true; // Helpful for Vulkan

    _swapChain = _system->CreateSwapChain(swapChainDesc, window.GetNative());
    
    // LOG the actual size to the console to debug tiny views
    auto actualRes = _swapChain->GetResolution();
    ZHLN::Log("Vulkan Surface Initialized: {}x{}\n", actualRes.width, actualRes.height);
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

    const auto& res = _swapChain->GetResolution();
    
    // 1. Viewport: Defines the coordinate mapping
    LLGL::Viewport viewport{ 0.0f, 0.0f, 
        static_cast<float>(res.width), 
        static_cast<float>(res.height) };
    _cmdBuffer->SetViewport(viewport);

    // 2. Scissor: Defines the clipping region (MANDATORY for Vulkan)
    LLGL::Scissor scissor{ 0, 0, 
        static_cast<long>(res.width), 
        static_cast<long>(res.height) };
    _cmdBuffer->SetScissor(scissor);
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
	auto* cmd = ctx.GetCommandBuffer();

	// 1. Bind the pipeline state (Shaders, Blend, Depth, etc.)
	cmd->SetPipelineState(*material.pipeline);

	// 2. Bind the resource heap (Contains the Global View-Proj Constant Buffer)
	cmd->SetResourceHeap(*material.resourceHeap);

	// 3. Bind the vertex geometry
	cmd->SetVertexBuffer(*mesh.vertexBuffer);

	// 4. Update the "model" matrix via Push Constants (Vulkan) / Uniforms (GL)
	// This replaces 'UpdateBuffer' and is legal to call inside a Render Pass.
	// Index 0 corresponds to the UniformDescriptor{"model", ...} in your layout.
	cmd->SetUniforms(0, &transform, sizeof(transform));

	// 5. Draw the mesh
	cmd->Draw(mesh.vertexCount, 0);
}

} // namespace Renderer
} // namespace ZHLN