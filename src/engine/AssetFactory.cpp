#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Render.hpp>
#include <fstream>
#include <sstream>

namespace ZHLN::AssetFactory {

static std::string LoadShaderRuntime(const std::string& path) {
	std::ifstream file(path);
	if (!file.is_open()) return "";
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
        // Front
        {{-x, -y, z}, color}, {{x, -y, z}, color}, {{x, y, z}, color},
        {{-x, -y, z}, color}, {{x, y, z}, color}, {{-x, y, z}, color},
        // Back
        {{x, -y, -z}, color}, {{-x, -y, -z}, color}, {{-x, y, -z}, color},
        {{x, -y, -z}, color}, {{-x, y, -z}, color}, {{x, y, -z}, color},
        // Left
        {{-x, -y, -z}, color}, {{-x, -y, z}, color}, {{-x, y, z}, color},
        {{-x, -y, -z}, color}, {{-x, y, z}, color}, {{-x, y, -z}, color},
        // Right
        {{x, -y, z}, color}, {{x, -y, -z}, color}, {{x, y, -z}, color},
        {{x, -y, z}, color}, {{x, y, -z}, color}, {{x, y, z}, color},
        // Top
        {{-x, y, z}, color}, {{x, y, z}, color}, {{x, y, -z}, color},
        {{-x, y, z}, color}, {{x, y, -z}, color}, {{-x, y, -z}, color},
        // Bottom
        {{-x, -y, -z}, color}, {{x, -y, -z}, color}, {{x, -y, z}, color},
        {{-x, -y, -z}, color}, {{x, -y, z}, color}, {{-x, -y, z}, color}
    };

	LLGL::BufferDescriptor vboDesc;
	vboDesc.size = sizeof(data);
	vboDesc.bindFlags = LLGL::BindFlags::VertexBuffer;
	auto* rawVbo = sys->CreateBuffer(vboDesc, data);
	return Mesh{.vertexBuffer = BufferPtr(rawVbo, LLGLDeleter{sys}), .vertexCount = 36};
}

Material CreateBasicMaterial(RenderContext& ctx) {
	LLGL::RenderSystem* sys = ctx.GetSystem();
	LLGLDeleter deleter{sys};
	
	std::string shaderSource;
	bool isMetal = (sys->GetRendererID() == LLGL::RendererID::Metal);

	if (isMetal) {
#if defined(__cpp_embed)
		static const char data[] = { #embed "../../resources/shaders/basic.metal" , 0 };
		shaderSource = data;
#else
		shaderSource = LoadShaderRuntime("resources/shaders/basic.metal");
#endif
	} else {
#if defined(__cpp_embed)
		static const char data[] = { #embed "../../resources/shaders/basic.glsl" , 0 };
		shaderSource = data;
#else
		shaderSource = LoadShaderRuntime("resources/shaders/basic.glsl");
#endif
	}

	LLGL::BufferDescriptor cboDesc;
	cboDesc.size = sizeof(FrameConstants);
	cboDesc.bindFlags = LLGL::BindFlags::ConstantBuffer;
	auto* rawCbo = sys->CreateBuffer(cboDesc);
	BufferPtr cbo(rawCbo, deleter);

	LLGL::BindingDescriptor bind;
	bind.name = "Constants";
	bind.type = LLGL::ResourceType::Buffer;
	bind.bindFlags = LLGL::BindFlags::ConstantBuffer;
	bind.stageFlags = LLGL::StageFlags::VertexStage;
	bind.slot = 1;

	LLGL::PipelineLayoutDescriptor layoutDesc;
	layoutDesc.heapBindings = {bind};
	LayoutPtr layout(sys->CreatePipelineLayout(layoutDesc), deleter);

	LLGL::ResourceViewDescriptor view;
	view.resource = cbo.get();
	LLGL::ResourceHeapDescriptor heapDesc;
	heapDesc.pipelineLayout = layout.get();
	HeapPtr heap(sys->CreateResourceHeap(heapDesc, {&view, 1}), deleter);

	LLGL::ShaderDescriptor vsDesc;
	vsDesc.type = LLGL::ShaderType::Vertex;
	vsDesc.source = shaderSource.c_str();
	vsDesc.sourceType = LLGL::ShaderSourceType::CodeString;
	vsDesc.entryPoint = isMetal ? "vsMain" : "main";
	vsDesc.profile = isMetal ? "1.1" : "450";
	vsDesc.vertex.inputAttribs = {
		{"position", LLGL::Format::RGB32Float, 0, offsetof(Vertex, position), sizeof(Vertex)},
		{"color", LLGL::Format::RGBA32Float, 1, offsetof(Vertex, color), sizeof(Vertex)}};

	LLGL::ShaderDescriptor fsDesc;
	fsDesc.type = LLGL::ShaderType::Fragment;
	fsDesc.source = shaderSource.c_str();
	fsDesc.sourceType = LLGL::ShaderSourceType::CodeString;
	fsDesc.entryPoint = isMetal ? "fsMain" : "main";
	fsDesc.profile = isMetal ? "1.1" : "450";

	auto* vs = sys->CreateShader(vsDesc);
	auto* fs = sys->CreateShader(fsDesc);

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

	return Material{.pipeline = std::move(pipeline),
					.layout = std::move(layout),
					.resourceHeap = std::move(heap),
					.constantBuffer = std::move(cbo)};
}

} // namespace ZHLN::AssetFactory