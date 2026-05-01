#include "engine/Render.hpp"

#include <utility>

namespace ZHLN {

// Shader Source (Metal for macOS)
constexpr const char* mslSource = R"(
#include <metal_stdlib>
using namespace metal;

struct Constants {
    float4x4 transform;
};

struct VertexIn {
    float3 position [[attribute(0)]];
    float4 color    [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float4 color;
};

// Bind the constant buffer to buffer slot 1
vertex VertexOut vsMain(VertexIn in [[stage_in]], constant Constants& cBuffer [[buffer(1)]]) {
    VertexOut out;
    out.position = cBuffer.transform * float4(in.position, 1.0);
    out.color = in.color;
    return out;
}

fragment float4 fsMain(VertexOut in [[stage_in]]) {
    return in.color;
}
)";

Renderer::Renderer(const String32& preferredAPI, uint32_t width, uint32_t height) {
	_system = LLGL::RenderSystem::Load(LLGL::RenderSystemDescriptor{preferredAPI.c_str()});
	if (!_system)
		return;

	_window = LLGL::Window::Create(
		LLGL::WindowDescriptor{.title = "Project-Zahlen Triangle",
							   .position = {},
							   .size = {width, height},
							   .flags = LLGL::WindowFlags::Visible | LLGL::WindowFlags::Centered});

	_swapChain = _system->CreateSwapChain({.resolution = _window->GetSize()}, _window);
	_commandQueue = _system->GetCommandQueue();
	_cmdBuffer = std::unique_ptr<LLGL::CommandBuffer>(_system->CreateCommandBuffer());

	CreatePipeline();
}

void Renderer::CreatePipeline() {
	Vertex triangle[] = {{{0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
						 {{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
						 {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}}};

	_vertexBuffer =
		_system->CreateBuffer(LLGL::BufferDescriptor{.size = sizeof(triangle),
													 .bindFlags = LLGL::BindFlags::VertexBuffer,
													 .vertexAttribs = {}},
							  triangle);

	// 2. Create the Constant Buffer
	_constantBuffer =
		_system->CreateBuffer(LLGL::BufferDescriptor{.size = sizeof(FrameConstants),
													 .bindFlags = LLGL::BindFlags::ConstantBuffer,
													 .vertexAttribs = {}});

	// 3. Create Pipeline Layout (Tells the shader to expect a Constant Buffer at slot 1)
	LLGL::BindingDescriptor bindingDesc;
	bindingDesc.name = "Constants";
	bindingDesc.type = LLGL::ResourceType::Buffer;
	bindingDesc.bindFlags = LLGL::BindFlags::ConstantBuffer;
	bindingDesc.stageFlags = LLGL::StageFlags::VertexStage;
	bindingDesc.slot = 1;

	_pipelineLayout = _system->CreatePipelineLayout(LLGL::PipelineLayoutDescriptor{
		// FIX: Move the binding to heapBindings so the ResourceHeap can use it
		.heapBindings = {bindingDesc},
		.bindings = {},
		.staticSamplers = {},
		.uniforms = {},
		.combinedTextureSamplers = {}});

	// 4. Create Resource Heap (Binds the physical buffer to the layout)
	LLGL::ResourceViewDescriptor viewDesc;
	viewDesc.resource = _constantBuffer;

	LLGL::ResourceHeapDescriptor heapDesc;
	heapDesc.pipelineLayout = _pipelineLayout;
	// Pass the array view: { pointer to viewDesc, number of elements }
	_resourceHeap = _system->CreateResourceHeap(heapDesc, {&viewDesc, 1});

	// 5. Shader Setup
	LLGL::ShaderDescriptor vsDesc;
	vsDesc.type = LLGL::ShaderType::Vertex;
	vsDesc.source = mslSource;
	vsDesc.sourceType = LLGL::ShaderSourceType::CodeString;
	vsDesc.entryPoint = "vsMain";
	vsDesc.profile = "1.1";
	vsDesc.vertex.inputAttribs = {
		{"position", LLGL::Format::RGB32Float, 0, offsetof(Vertex, position), sizeof(Vertex)},
		{"color", LLGL::Format::RGBA32Float, 1, offsetof(Vertex, color), sizeof(Vertex)}};

	LLGL::ShaderDescriptor fsDesc;
	fsDesc.type = LLGL::ShaderType::Fragment;
	fsDesc.source = mslSource;
	fsDesc.sourceType = LLGL::ShaderSourceType::CodeString;
	fsDesc.entryPoint = "fsMain";
	fsDesc.profile = "1.1";

	auto* vertexShader = _system->CreateShader(vsDesc);
	auto* fragmentShader = _system->CreateShader(fsDesc);

	// 6. Pipeline Setup (Now includes the .pipelineLayout)
	_pipeline = _system->CreatePipelineState(
		LLGL::GraphicsPipelineDescriptor{.pipelineLayout = _pipelineLayout, // Bind the layout here!
										 .renderPass = _swapChain->GetRenderPass(),
										 .vertexShader = vertexShader,
										 .fragmentShader = fragmentShader,
										 .primitiveTopology = LLGL::PrimitiveTopology::TriangleList,
										 .viewports = {},
										 .scissors = {},
										 .depth = {},
										 .stencil = {},
										 .rasterizer = {},
										 .blend = {},
										 .tessellation = {}});

	_system->Release(*vertexShader);
	_system->Release(*fragmentShader);
}

void Renderer::DrawTriangle(const JPH::Mat44& transform) {
	// 1. Update Constant Buffer Data
	FrameConstants constants;
	constants.transform = transform;
	_system->WriteBuffer(*_constantBuffer, 0, &constants, sizeof(constants));

	// 2. Bind everything and Draw
	_cmdBuffer->SetVertexBuffer(*_vertexBuffer);
	_cmdBuffer->SetPipelineState(*_pipeline);
	_cmdBuffer->SetResourceHeap(*_resourceHeap); // Bind the resource heap!
	_cmdBuffer->Draw(3, 0);
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

void Renderer::EndFrame() {
	_cmdBuffer->EndRenderPass();
	_cmdBuffer->End();
	_commandQueue->Submit(*_cmdBuffer);
	_swapChain->Present();
}

void Renderer::Clear(const JPH::Vec4& color) {
	LLGL::ClearValue val;
	using Ch = ColorComponent;
	val.color[std::to_underlying(Ch::R)] = color.GetX();
	val.color[std::to_underlying(Ch::G)] = color.GetY();
	val.color[std::to_underlying(Ch::B)] = color.GetZ();
	val.color[std::to_underlying(Ch::A)] = color.GetW();
	_cmdBuffer->Clear(LLGL::ClearFlags::Color, val);
}

Renderer::~Renderer() {
	if (_system) {
		// Cleanup new resources
		_system->Release(*_constantBuffer);
		_system->Release(*_pipelineLayout);
		_system->Release(*_resourceHeap);

		_system->Release(*_vertexBuffer);
		_system->Release(*_pipeline);
		LLGL::RenderSystem::Unload(std::move(_system));
	}
}

} // namespace ZHLN