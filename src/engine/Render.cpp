#include <Zahlen/Render.hpp>
#include <Zahlen/Log.hpp>
#include <LLGL/LLGL.h>
#include <Jolt/Core/Core.h> // For JPH::Array
#include <memory>

namespace ZHLN {

struct RenderContext::Impl {
	LLGL::RenderSystemPtr system;
	LLGL::CommandBuffer* cmdBuffer = nullptr; // Managed manually in ~Impl
	LLGL::SwapChain* swapChain = nullptr;
	LLGL::CommandQueue* commandQueue = nullptr;

	// Deletion Queue
	JPH::Array<LLGL::Buffer*> buffers;
	JPH::Array<LLGL::PipelineLayout*> layouts;
	JPH::Array<LLGL::ResourceHeap*> heaps;
	JPH::Array<LLGL::PipelineState*> pipelines;

	~Impl() {
		// Must release the command buffer before unloading the system
		if (cmdBuffer) system->Release(*cmdBuffer);
		
		for(auto* p : pipelines) system->Release(*p);
		for(auto* h : heaps) system->Release(*h);
		for(auto* l : layouts) system->Release(*l);
		for(auto* b : buffers) system->Release(*b);
		
		LLGL::RenderSystem::Unload(std::move(system));
	}
};

RenderContext::RenderContext(Window& window, const String32& preferredAPI) : _impl(std::make_unique<Impl>()) {
	auto modules = LLGL::RenderSystem::FindModules();
	String32 selected = "";
	for (const auto& m : modules) if (m == preferredAPI.c_str()) { selected = m.c_str(); break; }
	if (selected.empty() && !modules.empty()) selected = modules[0].c_str();

	_impl->system = LLGL::RenderSystem::Load(LLGL::RenderSystemDescriptor{selected.c_str()});
	
	// Retrieve the native OS window safely without violating the Pimpl boundary
	auto* rawNative = static_cast<LLGL::Surface*>(window.GetNativeHandle());
	auto nativeWin = std::shared_ptr<LLGL::Surface>(rawNative, [](LLGL::Surface*){});

	LLGL::SwapChainDescriptor swapChainDesc;
	swapChainDesc.resolution = { window.GetSize().width, window.GetSize().height };
	swapChainDesc.depthBits = 32;
	swapChainDesc.samples = 8;
	swapChainDesc.resizable = true;

	_impl->swapChain = _impl->system->CreateSwapChain(swapChainDesc, nativeWin);
	_impl->commandQueue = _impl->system->GetCommandQueue();
	_impl->cmdBuffer = _impl->system->CreateCommandBuffer();
}

RenderContext::~RenderContext() = default;

const char* RenderContext::GetRendererName() const {
    return _impl->system->GetName();
}

void RenderContext::BeginFrame() {
	_impl->cmdBuffer->Begin();
	_impl->cmdBuffer->BeginRenderPass(*_impl->swapChain);
	const auto& res = _impl->swapChain->GetResolution();
	_impl->cmdBuffer->SetViewport(LLGL::Viewport{0, 0, static_cast<float>(res.width), static_cast<float>(res.height)});
	_impl->cmdBuffer->SetScissor(LLGL::Scissor{0, 0, static_cast<long>(res.width), static_cast<long>(res.height)});
}

void RenderContext::EndFrame() {
	_impl->cmdBuffer->EndRenderPass();
	_impl->cmdBuffer->End();
	_impl->commandQueue->Submit(*_impl->cmdBuffer);
	_impl->swapChain->Present();
}

void RenderContext::SetResolution(const Extent2D& res) {
	if (res.width > 0 && res.height > 0) {
		_impl->swapChain->ResizeBuffers({res.width, res.height});
	}
}

// --- Resource Creation ---

BufferHandle RenderContext::CreateVertexBuffer(const void* data, size_t size) {
	LLGL::BufferDescriptor desc;
	desc.size = size;
	desc.bindFlags = LLGL::BindFlags::VertexBuffer;
	auto* buf = _impl->system->CreateBuffer(desc, data);
	_impl->buffers.push_back(buf);
	return static_cast<BufferHandle>(reinterpret_cast<uint64_t>(buf));
}

BufferHandle RenderContext::CreateConstantBuffer(size_t size) {
	LLGL::BufferDescriptor desc;
	desc.size = size;
	desc.bindFlags = LLGL::BindFlags::ConstantBuffer;
	auto* buf = _impl->system->CreateBuffer(desc);
	_impl->buffers.push_back(buf);
	return static_cast<BufferHandle>(reinterpret_cast<uint64_t>(buf));
}

Material RenderContext::CreateMaterial(const PipelineDesc& desc) {
	Material mat;

	// Constant Buffer
	mat.constantBuffer = CreateConstantBuffer(sizeof(FrameConstants));
	auto* rawCbo = reinterpret_cast<LLGL::Buffer*>(mat.constantBuffer);

	// Layout
	LLGL::BindingDescriptor bind{"Constants", LLGL::ResourceType::Buffer, LLGL::BindFlags::ConstantBuffer, LLGL::StageFlags::VertexStage, 1};
	LLGL::PipelineLayoutDescriptor layoutDesc;
	layoutDesc.heapBindings = {bind};
	layoutDesc.uniforms = { LLGL::UniformDescriptor{"model", LLGL::UniformType::Float4x4} };
	auto* layout = _impl->system->CreatePipelineLayout(layoutDesc);
	_impl->layouts.push_back(layout);

	// Heap (ResourceGroup)
	LLGL::ResourceViewDescriptor view{rawCbo};
	LLGL::ResourceHeapDescriptor heapDesc{layout};
	auto* heap = _impl->system->CreateResourceHeap(heapDesc, {&view, 1});
	_impl->heaps.push_back(heap);
	mat.resourceGroup = static_cast<ResourceGroupHandle>(reinterpret_cast<uint64_t>(heap));

	// Shaders
	LLGL::ShaderDescriptor vsDesc, fsDesc;
	vsDesc.type = LLGL::ShaderType::Vertex; fsDesc.type = LLGL::ShaderType::Fragment;
	vsDesc.entryPoint = "main"; fsDesc.entryPoint = "main";

	if (desc.isMetal) {
		vsDesc.sourceType = LLGL::ShaderSourceType::CodeString;
		fsDesc.sourceType = LLGL::ShaderSourceType::CodeString;
		vsDesc.entryPoint = "vsMain"; fsDesc.entryPoint = "fsMain";
	} else {
		vsDesc.sourceType = LLGL::ShaderSourceType::BinaryBuffer;
		fsDesc.sourceType = LLGL::ShaderSourceType::BinaryBuffer;
	}

	vsDesc.source = reinterpret_cast<const char*>(desc.vertexShaderData);
	vsDesc.sourceSize = desc.vertexShaderSize;
	fsDesc.source = reinterpret_cast<const char*>(desc.fragShaderData);
	fsDesc.sourceSize = desc.fragShaderSize;

	vsDesc.vertex.inputAttribs = {
		{"position", LLGL::Format::RGB32Float, 0, offsetof(Vertex, position), sizeof(Vertex)},
		{"color", LLGL::Format::RGBA32Float, 1, offsetof(Vertex, color), sizeof(Vertex)}
	};

	auto* vs = _impl->system->CreateShader(vsDesc);
	auto* fs = _impl->system->CreateShader(fsDesc);

	if (!vs || !fs || !layout) {
		ZHLN::Log("CreateMaterial failed during shader/layout creation: vs={}, fs={}, layout={}\n",
			reinterpret_cast<std::uintptr_t>(vs), reinterpret_cast<std::uintptr_t>(fs), reinterpret_cast<std::uintptr_t>(layout));
	}

	// Pipeline
	LLGL::GraphicsPipelineDescriptor psoDesc;
	psoDesc.pipelineLayout = layout;
	psoDesc.renderPass = _impl->swapChain->GetRenderPass();
	psoDesc.vertexShader = vs;
	psoDesc.fragmentShader = fs;
	psoDesc.primitiveTopology = LLGL::PrimitiveTopology::TriangleList;
	psoDesc.depth.testEnabled = true; psoDesc.depth.writeEnabled = true;
	psoDesc.depth.compareOp = LLGL::CompareOp::Less;

	auto* pso = _impl->system->CreatePipelineState(psoDesc);
	if (!pso) {
		ZHLN::Log("CreateMaterial failed during pipeline creation: pso={}\n",
			reinterpret_cast<std::uintptr_t>(pso));
	}
	_impl->pipelines.push_back(pso);
	mat.pipeline = static_cast<PipelineHandle>(reinterpret_cast<uint64_t>(pso));

	_impl->system->Release(*vs);
	_impl->system->Release(*fs);

	return mat;
}

namespace Renderer {
	void Clear(RenderContext& ctx, const JPH::Vec4& color, float depth) {
		LLGL::ClearValue val;
		val.color[0] = color.GetX(); val.color[1] = color.GetY(); val.color[2] = color.GetZ(); val.color[3] = color.GetW();
		val.depth = depth;
		ctx.GetImpl()->cmdBuffer->Clear(LLGL::ClearFlags::Color | LLGL::ClearFlags::Depth, val);
	}

	void UpdateBuffer(RenderContext& ctx, BufferHandle buffer, const void* data, size_t size) {
		if (buffer == BufferHandle::Invalid) {
			return;
		}
		auto* raw = reinterpret_cast<LLGL::Buffer*>(static_cast<uint64_t>(buffer));
		ctx.GetImpl()->system->WriteBuffer(*raw, 0, data, size);
	}

	void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh, const JPH::Mat44& transform) {
		if (material.pipeline == PipelineHandle::Invalid || material.resourceGroup == ResourceGroupHandle::Invalid || mesh.vertexBuffer == BufferHandle::Invalid) {
			return;
		}
		auto* cmd = ctx.GetImpl()->cmdBuffer;
		cmd->SetPipelineState(*reinterpret_cast<LLGL::PipelineState*>(static_cast<uint64_t>(material.pipeline)));
		cmd->SetResourceHeap(*reinterpret_cast<LLGL::ResourceHeap*>(static_cast<uint64_t>(material.resourceGroup)));
		cmd->SetVertexBuffer(*reinterpret_cast<LLGL::Buffer*>(static_cast<uint64_t>(mesh.vertexBuffer)));
		cmd->SetUniforms(0, &transform, sizeof(transform));
		cmd->Draw(mesh.vertexCount, 0);
	}
}

} // namespace ZHLN