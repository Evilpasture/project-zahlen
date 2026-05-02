#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Resources.hpp>
#include <fstream>
#include <sstream>


namespace ZHLN::AssetFactory {

[[maybe_unused]] std::string LoadShaderRuntime(const std::string& path) {
	std::ifstream file(path);
	if (!file.is_open())
		return "";
	std::stringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}

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

	Vertex data[] = {
		{{-x, -y, z}, color},  {{x, -y, z}, color},	  {{x, y, z}, color},	 {{-x, -y, z}, color},
		{{x, y, z}, color},	   {{-x, y, z}, color},	  {{x, -y, -z}, color},	 {{-x, -y, -z}, color},
		{{-x, y, -z}, color},  {{x, -y, -z}, color},  {{-x, y, -z}, color},	 {{x, y, -z}, color},
		{{-x, -y, -z}, color}, {{-x, -y, z}, color},  {{-x, y, z}, color},	 {{-x, -y, -z}, color},
		{{-x, y, z}, color},   {{-x, y, -z}, color},  {{x, -y, z}, color},	 {{x, -y, -z}, color},
		{{x, y, -z}, color},   {{x, -y, z}, color},	  {{x, y, -z}, color},	 {{x, y, z}, color},
		{{-x, y, z}, color},   {{x, y, z}, color},	  {{x, y, -z}, color},	 {{-x, y, z}, color},
		{{x, y, -z}, color},   {{-x, y, -z}, color},  {{-x, -y, -z}, color}, {{x, -y, -z}, color},
		{{x, -y, z}, color},   {{-x, -y, -z}, color}, {{x, -y, z}, color},	 {{-x, -y, z}, color}};

	LLGL::BufferDescriptor vboDesc;
	vboDesc.size = sizeof(data);
	vboDesc.bindFlags = LLGL::BindFlags::VertexBuffer;
	auto* rawVbo = sys->CreateBuffer(vboDesc, data);
	return Mesh{.vertexBuffer = BufferPtr(rawVbo, LLGLDeleter{sys}), .vertexCount = 36};
}

Material CreateBasicMaterial(RenderContext& ctx) {
	LLGL::RenderSystem* sys = ctx.GetSystem();
	LLGLDeleter deleter{sys};

	const auto renderer = sys->GetRendererID();
	const bool isVulkan = (renderer == LLGL::RendererID::Vulkan);
	const bool isMetal = (renderer == LLGL::RendererID::Metal);

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
	LayoutPtr layout(sys->CreatePipelineLayout(layoutDesc), deleter);

	// 3. Heap
	LLGL::ResourceViewDescriptor view;
	view.resource = cbo.get();
	LLGL::ResourceHeapDescriptor heapDesc;
	heapDesc.pipelineLayout = layout.get();
	HeapPtr heap(sys->CreateResourceHeap(heapDesc, {&view, 1}), deleter);

	// 4. Shader Selection and Loading
	LLGL::ShaderDescriptor vsDesc, fsDesc;
	vsDesc.type = LLGL::ShaderType::Vertex;
	fsDesc.type = LLGL::ShaderType::Fragment;

	if (isVulkan) {
		// Use reinterpret_cast to bridge the unsigned/signed char gap for binary data
		vsDesc.source = reinterpret_cast<const char*>(ZHLN_Resource_BasicVertSpv);
		vsDesc.sourceSize = ZHLN_Resource_BasicVertSpv_Len;
		vsDesc.sourceType = LLGL::ShaderSourceType::BinaryBuffer;

		fsDesc.source = reinterpret_cast<const char*>(ZHLN_Resource_BasicFragSpv);
		fsDesc.sourceSize = ZHLN_Resource_BasicFragSpv_Len;
		fsDesc.sourceType = LLGL::ShaderSourceType::BinaryBuffer;

		vsDesc.entryPoint = "main";
		fsDesc.entryPoint = "main";
	} else if (isMetal) {
		// Metal is already const char[] in Resources.hpp, so no cast is needed here
		vsDesc.source = ZHLN_Resource_BasicMetal;
		vsDesc.sourceType = LLGL::ShaderSourceType::CodeString;

		fsDesc.source = ZHLN_Resource_BasicMetal;
		fsDesc.sourceType = LLGL::ShaderSourceType::CodeString;

		vsDesc.entryPoint = "vsMain";
		fsDesc.entryPoint = "fsMain";
	}

	vsDesc.vertex.inputAttribs = {
		{"position", LLGL::Format::RGB32Float, 0, offsetof(Vertex, position), sizeof(Vertex)},
		{"color", LLGL::Format::RGBA32Float, 1, offsetof(Vertex, color), sizeof(Vertex)}};

	auto* vs = sys->CreateShader(vsDesc);
	auto* fs = sys->CreateShader(fsDesc);

	// 5. Graphics Pipeline
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
	if (!rawPso) {
		if (auto* report = sys->GetReport()) {
			ZHLN::Log("Pipeline State Error:\n{}\n", report->GetText());
		}
	}

	// Shaders are now baked into the PSO, we can release them
	sys->Release(*vs);
	sys->Release(*fs);

	return Material{.pipeline = PipelinePtr(rawPso, deleter),
					.layout = std::move(layout),
					.resourceHeap = std::move(heap),
					.constantBuffer = std::move(cbo)};
}

} // namespace ZHLN::AssetFactory