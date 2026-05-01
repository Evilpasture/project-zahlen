#include "engine/Render.hpp"

#include <utility>

namespace ZHLN {

// Shader Source (Metal for macOS)
const char* mslSource = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float4 color    [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float4 color;
};

vertex VertexOut vsMain(VertexIn in [[stage_in]]) {
    VertexOut out;
    out.position = float4(in.position, 1.0);
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
	// 1. Define Geometry (Jolt Math)
	Vertex triangle[] = {
		{{0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},	 // Top (Red)
		{{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}}, // Right (Green)
		{{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}} // Left (Blue)
	};

	// 2. Create Vertex Buffer using Designated Initializers
	_vertexBuffer =
		_system->CreateBuffer(LLGL::BufferDescriptor{.size = sizeof(triangle),
													 .bindFlags = LLGL::BindFlags::VertexBuffer,
													 .vertexAttribs = {}},
							  triangle);

	// 3. Create Shaders with Vertex Attributes injected directly into the VS
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

	// 4. Create Pipeline State using Designated Initializers
	_pipeline = _system->CreatePipelineState(
		LLGL::GraphicsPipelineDescriptor{.renderPass = _swapChain->GetRenderPass(),
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

	// Optional: Since the pipeline is compiled, we can release the shaders
	// immediately to free GPU memory, or let the RenderSystem clean them up on exit.
	_system->Release(*vertexShader);
	_system->Release(*fragmentShader);
}

void Renderer::DrawTriangle() {
	_cmdBuffer->SetVertexBuffer(*_vertexBuffer);
	_cmdBuffer->SetPipelineState(*_pipeline);
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
		_system->Release(*_vertexBuffer);
		_system->Release(*_pipeline);
		LLGL::RenderSystem::Unload(std::move(_system));
	}
}

} // namespace ZHLN