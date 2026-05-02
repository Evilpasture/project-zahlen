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
	// Free all GPU assets in reverse order of creation
	for (auto it = _cleanupQueue.rbegin(); it != _cleanupQueue.rend(); ++it) {
		(*it)();
	}
	_context.reset(); // Destroy GPU connection

	// Destroy Physics
	JPH::UnregisterTypes();
	delete JPH::Factory::sInstance;
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

	LLGL::BufferDescriptor vboDesc;
	vboDesc.size = sizeof(data);
	vboDesc.bindFlags = LLGL::BindFlags::VertexBuffer;

	LLGL::Buffer* vbo = _context->GetSystem()->CreateBuffer(vboDesc, data);

	_cleanupQueue.push_back([sys = _context->GetSystem(), vbo]() { sys->Release(*vbo); });

	return Mesh{.vertexBuffer = vbo, .vertexCount = 12};
}

Material Engine::CreateMaterial() {
	LLGL::RenderSystem* system = _context->GetSystem();
	Material mat;

	// 1. Constant Buffer
	LLGL::BufferDescriptor cboDesc;
	cboDesc.size = sizeof(FrameConstants);
	cboDesc.bindFlags = LLGL::BindFlags::ConstantBuffer;
	mat.constantBuffer = system->CreateBuffer(cboDesc);

	// 2. Layout (Manual Assignment fixes non-aggregate error)
	LLGL::BindingDescriptor bindingDesc;
	bindingDesc.name = "Constants";
	bindingDesc.type = LLGL::ResourceType::Buffer;
	bindingDesc.bindFlags = LLGL::BindFlags::ConstantBuffer;
	bindingDesc.stageFlags = LLGL::StageFlags::VertexStage;
	bindingDesc.slot = 1;

	LLGL::PipelineLayoutDescriptor layoutDesc;
	layoutDesc.heapBindings = {bindingDesc};
	mat.layout = system->CreatePipelineLayout(layoutDesc);

	// 3. Heap (Manual Assignment)
	LLGL::ResourceViewDescriptor viewDesc;
	viewDesc.resource = mat.constantBuffer;

	LLGL::ResourceHeapDescriptor heapDesc;
	heapDesc.pipelineLayout = mat.layout;
	mat.resourceHeap = system->CreateResourceHeap(heapDesc, {&viewDesc, 1});

	// 4. Shaders (Hardcoded for now)
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
	psoDesc.pipelineLayout = mat.layout;
	psoDesc.renderPass = _context->GetSwapChain()->GetRenderPass();
	psoDesc.vertexShader = vs;
	psoDesc.fragmentShader = fs;
	psoDesc.primitiveTopology = LLGL::PrimitiveTopology::TriangleList;
	psoDesc.depth.testEnabled = true;
	psoDesc.depth.writeEnabled = true;
	psoDesc.depth.compareOp = LLGL::CompareOp::Less;
	mat.pipeline = system->CreatePipelineState(psoDesc);

	system->Release(*vs);
	system->Release(*fs);

	// Register Cleanup
	_cleanupQueue.push_back([system, mat]() {
		system->Release(*mat.constantBuffer);
		system->Release(*mat.layout);
		system->Release(*mat.resourceHeap);
		system->Release(*mat.pipeline);
	});

	return mat;
}

} // namespace ZHLN