#include "engine/AssetFactory.hpp"

#include "engine/Render.hpp" // For RenderContext access

namespace ZHLN::AssetFactory {

Mesh CreateTetrahedron(RenderContext& ctx) {
	LLGL::RenderSystem* sys = ctx.GetSystem();

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

	auto* rawVbo = sys->CreateBuffer(vboDesc, data);

	// Fixed: Use explicit constructor for BufferPtr
	return Mesh{.vertexBuffer = BufferPtr(rawVbo, LLGLDeleter{sys}), .vertexCount = 12};
}

Mesh CreatePlane(RenderContext& ctx, float extent, const JPH::Vec4& color) {
	LLGL::RenderSystem* sys = ctx.GetSystem();

	Vertex data[] = {{{-extent, 0.0f, -extent}, color}, {{extent, 0.0f, -extent}, color},
					 {{extent, 0.0f, extent}, color},	{{-extent, 0.0f, -extent}, color},
					 {{extent, 0.0f, extent}, color},	{{-extent, 0.0f, extent}, color}};

	LLGL::BufferDescriptor vboDesc;
	vboDesc.size = sizeof(data);
	vboDesc.bindFlags = LLGL::BindFlags::VertexBuffer;
	auto* rawVbo = sys->CreateBuffer(vboDesc, data);
	return Mesh{.vertexBuffer = BufferPtr(rawVbo, LLGLDeleter{sys}), .vertexCount = 6};
}

Mesh CreateBox(RenderContext& ctx, JPH::Vec3Arg halfExtents, const JPH::Vec4& color) {
	LLGL::RenderSystem* sys = ctx.GetSystem();

	const float x = halfExtents.GetX();
	const float y = halfExtents.GetY();
	const float z = halfExtents.GetZ();

	Vertex data[] = {// Front
					 {{-x, -y, z}, color},
					 {{x, -y, z}, color},
					 {{x, y, z}, color},
					 {{-x, -y, z}, color},
					 {{x, y, z}, color},
					 {{-x, y, z}, color},
					 // Back
					 {{x, -y, -z}, color},
					 {{-x, -y, -z}, color},
					 {{-x, y, -z}, color},
					 {{x, -y, -z}, color},
					 {{-x, y, -z}, color},
					 {{x, y, -z}, color},
					 // Left
					 {{-x, -y, -z}, color},
					 {{-x, -y, z}, color},
					 {{-x, y, z}, color},
					 {{-x, -y, -z}, color},
					 {{-x, y, z}, color},
					 {{-x, y, -z}, color},
					 // Right
					 {{x, -y, z}, color},
					 {{x, -y, -z}, color},
					 {{x, y, -z}, color},
					 {{x, -y, z}, color},
					 {{x, y, -z}, color},
					 {{x, y, z}, color},
					 // Top
					 {{-x, y, z}, color},
					 {{x, y, z}, color},
					 {{x, y, -z}, color},
					 {{-x, y, z}, color},
					 {{x, y, -z}, color},
					 {{-x, y, -z}, color},
					 // Bottom
					 {{-x, -y, -z}, color},
					 {{x, -y, -z}, color},
					 {{x, -y, z}, color},
					 {{-x, -y, -z}, color},
					 {{x, -y, z}, color},
					 {{-x, -y, z}, color}};

	LLGL::BufferDescriptor vboDesc;
	vboDesc.size = sizeof(data);
	vboDesc.bindFlags = LLGL::BindFlags::VertexBuffer;
	auto* rawVbo = sys->CreateBuffer(vboDesc, data);
	return Mesh{.vertexBuffer = BufferPtr(rawVbo, LLGLDeleter{sys}), .vertexCount = 36};
}

Material CreateBasicMaterial(RenderContext& ctx) {
	LLGL::RenderSystem* sys = ctx.GetSystem();
	LLGLDeleter deleter{sys};

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

	// 1. Constant Buffer
	LLGL::BufferDescriptor cboDesc;
	cboDesc.size = sizeof(FrameConstants);
	cboDesc.bindFlags = LLGL::BindFlags::ConstantBuffer;
	auto* rawCbo = sys->CreateBuffer(cboDesc);
	BufferPtr cbo(rawCbo, deleter);

	// 2. Layout
	LLGL::BindingDescriptor bind;
	bind.name = "Constants";
	bind.type = LLGL::ResourceType::Buffer;
	bind.bindFlags = LLGL::BindFlags::ConstantBuffer;
	bind.stageFlags = LLGL::StageFlags::VertexStage;
	bind.slot = 1;

	LLGL::PipelineLayoutDescriptor layoutDesc;
	layoutDesc.heapBindings = {bind};
	auto* rawLayout = sys->CreatePipelineLayout(layoutDesc);
	LayoutPtr layout(rawLayout, deleter);

	// 3. Heap (Manual assignment to satisfy non-aggregate)
	LLGL::ResourceViewDescriptor view;
	view.resource = cbo.get();

	LLGL::ResourceHeapDescriptor heapDesc;
	heapDesc.pipelineLayout = layout.get();
	auto* rawHeap = sys->CreateResourceHeap(heapDesc, {&view, 1});
	HeapPtr heap(rawHeap, deleter);

	// 4. Shaders
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

	auto* vs = sys->CreateShader(vsDesc);
	auto* fs = sys->CreateShader(fsDesc);

	// 5. Pipeline
	LLGL::GraphicsPipelineDescriptor psoDesc;
	psoDesc.pipelineLayout = layout.get();
	psoDesc.renderPass = ctx.GetSwapChain()->GetRenderPass();
	psoDesc.vertexShader = vs;
	psoDesc.fragmentShader = fs;
	psoDesc.primitiveTopology = LLGL::PrimitiveTopology::TriangleList;
	psoDesc.depth.testEnabled = true;
	psoDesc.depth.writeEnabled = true;
	psoDesc.depth.compareOp = LLGL::CompareOp::Less;

	auto* rawPso = sys->CreatePipelineState(psoDesc);
	sys->Release(*vs);
	sys->Release(*fs);

	return Material{.pipeline = PipelinePtr(rawPso, deleter),
					.layout = std::move(layout),
					.resourceHeap = std::move(heap),
					.constantBuffer = std::move(cbo)};
}

} // namespace ZHLN::AssetFactory