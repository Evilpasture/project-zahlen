// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include "engine/Engine.hpp"
#include "engine/Log.hpp"
// clang-format on

namespace ZHLN {

extern void JoltTraceBridge(const char* inFMT, ...) noexcept;
extern bool JoltAssertBridge(const char* inExpression, const char* inMessage, const char* inFile,
							 uint32_t inLine) noexcept;

Engine::Engine() {
	// 1. Initialize Physics
	JPH::RegisterDefaultAllocator();
	JPH::Trace = JoltTraceBridge;
#ifdef JPH_ENABLE_ASSERTS
	JPH::AssertFailed = JoltAssertBridge;
#endif
	JPH::Factory::sInstance = new JPH::Factory();
	JPH::RegisterTypes();

	// 2. Initialize Graphics
	_context = std::make_unique<RenderContext>("Metal", 1280, 720);
}

Engine::~Engine() {
	// 1. The _cleanupQueue is gone!
	// Assets (Mesh/Material) owned by the user (main.cpp) now handle their own
	// destruction via the LLGLDeleter. As long as the assets are declared
	// AFTER the engine in main(), they will be destroyed BEFORE the engine.

	// 2. Destroy the RenderContext (GPU Connection & Window)
	_context.reset();

	// 3. Destroy Jolt Physics Globals
	JPH::UnregisterTypes();

	if (JPH::Factory::sInstance != nullptr) {
		delete JPH::Factory::sInstance;
		JPH::Factory::sInstance = nullptr;
	}
}

bool Engine::IsRunning() const {
	return _context->IsRunning();
}
void Engine::ProcessEvents() {
	_context->ProcessEvents();
}
void Engine::BeginFrame() {
	_context->BeginFrame();
}
void Engine::EndFrame() {
	_context->EndFrame();
}

Mesh Engine::CreateTetrahedron() {
	LLGL::RenderSystem* sys = _context->GetSystem();
	Vertex data[] = {{{0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
					 {{0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f, 1.0f}},
					 {{-0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f, 1.0f}},
					 {{0.0f, 0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
					 {{0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 0.0f, 1.0f}},
					 {{0.5f, -0.5f, 0.5f}, {0.0f, 1.0f, 0.0f, 1.0f}},
					 {{0.0f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}},
					 {{-0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f, 1.0f}},
					 {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, 1.0f, 1.0f}},
					 {{-0.5f, -0.5f, 0.5f}, {1.0f, 1.0f, 0.0f, 1.0f}},
					 {{0.5f, -0.5f, 0.5f}, {1.0f, 1.0f, 0.0f, 1.0f}},
					 {{0.0f, -0.5f, -0.5f}, {1.0f, 1.0f, 0.0f, 1.0f}}};

	LLGL::BufferDescriptor vboDesc{
		.size = sizeof(data), .bindFlags = LLGL::BindFlags::VertexBuffer, .vertexAttribs = {}};

	// Create raw, then wrap in smart pointer
	auto* rawVbo = sys->CreateBuffer(vboDesc, data);

	return Mesh{.vertexBuffer = BufferPtr(rawVbo, LLGLDeleter{sys}), .vertexCount = 12};
}

Material Engine::CreateMaterial() {
	LLGL::RenderSystem* system = _context->GetSystem();
	LLGLDeleter deleter{system};

	// 1. Constant Buffer
	LLGL::BufferDescriptor cboDesc;
	cboDesc.size = sizeof(FrameConstants);
	cboDesc.bindFlags = LLGL::BindFlags::ConstantBuffer;

	auto* rawCbo = system->CreateBuffer(cboDesc);
	BufferPtr constantBuffer(rawCbo, deleter);

	// 2. Layout (Manual Assignment for Non-Aggregate)
	LLGL::BindingDescriptor bindingDesc;
	bindingDesc.name = "Constants";
	bindingDesc.type = LLGL::ResourceType::Buffer;
	bindingDesc.bindFlags = LLGL::BindFlags::ConstantBuffer;
	bindingDesc.stageFlags = LLGL::StageFlags::VertexStage;
	bindingDesc.slot = 1;

	LLGL::PipelineLayoutDescriptor layoutDesc;
	layoutDesc.heapBindings = {bindingDesc};

	auto* rawLayout = system->CreatePipelineLayout(layoutDesc);
	LayoutPtr layout(rawLayout, deleter);

	// 3. Heap (Manual Assignment for Non-Aggregate)
	LLGL::ResourceViewDescriptor viewDesc;
	viewDesc.resource = constantBuffer.get(); // Use .get() for the raw pointer

	LLGL::ResourceHeapDescriptor heapDesc;
	heapDesc.pipelineLayout = layout.get();

	auto* rawHeap = system->CreateResourceHeap(heapDesc, {&viewDesc, 1});
	HeapPtr resourceHeap(rawHeap, deleter);

	// 4. Shaders (Hardcoded MSL)
	const char* mslSource = R"(
	#include <metal_stdlib>
	using namespace metal;
	struct Constants { float4x4 transform; };
	struct VertexIn  { float3 position [[attribute(0)]]; float4 color [[attribute(1)]]; };
	struct VertexOut { float4 position [[position]]; float4 color; };
	vertex VertexOut vsMain(VertexIn in [[stage_in]], constant Constants& cBuffer [[buffer(1)]]) {
		VertexOut out; out.position = cBuffer.transform * float4(in.position, 1.0); out.color = in.color; return out;
	}
	fragment float4 fsMain(VertexOut in [[stage_in]]) { return in.color; }
	)";

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

	auto* vs = system->CreateShader(vsDesc);
	auto* fs = system->CreateShader(fsDesc);

	// 5. Pipeline State
	LLGL::GraphicsPipelineDescriptor psoDesc;
	psoDesc.pipelineLayout = layout.get();
	psoDesc.renderPass = _context->GetSwapChain()->GetRenderPass();
	psoDesc.vertexShader = vs;
	psoDesc.fragmentShader = fs;
	psoDesc.primitiveTopology = LLGL::PrimitiveTopology::TriangleList;
	psoDesc.depth.testEnabled = true;
	psoDesc.depth.writeEnabled = true;
	psoDesc.depth.compareOp = LLGL::CompareOp::Less;

	auto* rawPso = system->CreatePipelineState(psoDesc);
	PipelinePtr pipeline(rawPso, deleter);

	// Temporary shaders can be released immediately
	system->Release(*vs);
	system->Release(*fs);

	// Return everything moved into the Material struct
	return Material{.pipeline = std::move(pipeline),
					.layout = std::move(layout),
					.resourceHeap = std::move(resourceHeap),
					.constantBuffer = std::move(constantBuffer)};
}

} // namespace ZHLN