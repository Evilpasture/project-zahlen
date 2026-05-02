#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Render.hpp> // For RenderContext access
#include <Zahlen/Log.hpp>


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

	// 1. Constant Buffer
	LLGL::BufferDescriptor cboDesc;
	cboDesc.size = sizeof(FrameConstants);
	cboDesc.bindFlags = LLGL::BindFlags::ConstantBuffer;
	BufferPtr cbo(sys->CreateBuffer(cboDesc), deleter);

	// 2. Layout
	LLGL::BindingDescriptor bind;
	bind.name = "Constants";
	bind.type = LLGL::ResourceType::Buffer;
	bind.bindFlags = LLGL::BindFlags::ConstantBuffer;
	bind.stageFlags = LLGL::StageFlags::VertexStage;
	bind.slot = 1;

	LLGL::PipelineLayoutDescriptor layoutDesc;
	layoutDesc.heapBindings = {bind};
	LayoutPtr layout(sys->CreatePipelineLayout(layoutDesc), deleter);

	// 3. Heap
	LLGL::ResourceViewDescriptor view;
	view.resource = cbo.get();
	LLGL::ResourceHeapDescriptor heapDesc;
	heapDesc.pipelineLayout = layout.get();
	HeapPtr heap(sys->CreateResourceHeap(heapDesc, {&view, 1}), deleter);

	// --- 4. SHADER LOADING (CROSS PLATFORM) ---
	LLGL::ShaderDescriptor vsDesc;
	vsDesc.type = LLGL::ShaderType::Vertex;
	vsDesc.sourceType = LLGL::ShaderSourceType::CodeFile; // Load from disk!
	vsDesc.entryPoint = "vsMain";
	vsDesc.vertex.inputAttribs = {
		{"position", LLGL::Format::RGB32Float, 0, offsetof(Vertex, position), sizeof(Vertex)},
		{"color", LLGL::Format::RGBA32Float, 1, offsetof(Vertex, color), sizeof(Vertex)}};

	LLGL::ShaderDescriptor fsDesc;
	fsDesc.type = LLGL::ShaderType::Fragment;
	fsDesc.sourceType = LLGL::ShaderSourceType::CodeFile;
	fsDesc.entryPoint = "fsMain";

	if (sys->GetRendererID() == LLGL::RendererID::Metal) {
		vsDesc.source = "resources/shaders/Basic.metal";
		vsDesc.profile = "1.1";
		fsDesc.source = "resources/shaders/Basic.metal";
		fsDesc.profile = "1.1";
	} else if (sys->GetRendererID() == LLGL::RendererID::Direct3D11 ||
			   sys->GetRendererID() == LLGL::RendererID::Direct3D12) {
		vsDesc.source = "resources/shaders/Basic.hlsl";
		vsDesc.profile = "vs_5_0";
		fsDesc.source = "resources/shaders/Basic.hlsl";
		fsDesc.profile = "ps_5_0";
	}

	auto* vs = sys->CreateShader(vsDesc);
	auto* fs = sys->CreateShader(fsDesc);

	// Error checking - extremely helpful if you have a typo in your shader file
	if (vs->GetReport() && vs->GetReport()->HasErrors()) {
        ZHLN::Log("Vertex Shader Error: {}\n", vs->GetReport()->GetText());
    }
    if (fs->GetReport() && fs->GetReport()->HasErrors()) {
        ZHLN::Log("Fragment Shader Error: {}\n", fs->GetReport()->GetText());
    }

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

	PipelinePtr pipeline(sys->CreatePipelineState(psoDesc), deleter);
	sys->Release(*vs);
	sys->Release(*fs);

	return Material{std::move(pipeline), std::move(layout), std::move(heap), std::move(cbo)};
}

} // namespace ZHLN::AssetFactory