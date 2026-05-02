// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Math/Mat44.h>

#include "engine/Render.hpp"
#include "engine/Log.hpp"
// clang-format on

using namespace ZHLN;

constexpr const char* mslSource = R"(
#include <metal_stdlib>
using namespace metal;

struct Constants { float4x4 transform; };
struct VertexIn  { float3 position [[attribute(0)]]; float4 color [[attribute(1)]]; };
struct VertexOut { float4 position [[position]]; float4 color; };

vertex VertexOut vsMain(VertexIn in [[stage_in]], constant Constants& cBuffer [[buffer(1)]]) {
    VertexOut out;
    out.position = cBuffer.transform * float4(in.position, 1.0);
    out.color = in.color;
    return out;
}

fragment float4 fsMain(VertexOut in [[stage_in]]) { return in.color; }
)";

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
	JPH::RegisterDefaultAllocator();
	JPH::Trace = ZHLN::JoltTraceBridge;
#ifdef JPH_ENABLE_ASSERTS
	JPH::AssertFailed = ZHLN::JoltAssertBridge;
#endif
	JPH::Factory::sInstance = new JPH::Factory();
	JPH::RegisterTypes();

	{
		// 1. Initialize Context
		RenderContext ctx("Metal", 1280, 720);
		LLGL::RenderSystem* system = ctx.GetSystem();

		// 2. Define User Data (Assets)
		Vertex triangle[] = {{{0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
							 {{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
							 {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}}};

		LLGL::Buffer* vbo =
			system->CreateBuffer(LLGL::BufferDescriptor{.size = sizeof(triangle),
														.bindFlags = LLGL::BindFlags::VertexBuffer,
														.vertexAttribs = {}},
								 triangle);
		LLGL::Buffer* cbo = system->CreateBuffer(
			LLGL::BufferDescriptor{.size = sizeof(FrameConstants),
								   .bindFlags = LLGL::BindFlags::ConstantBuffer,
								   .vertexAttribs = {}});

		LLGL::BindingDescriptor bindingDesc;
		bindingDesc.name = "Constants";
		bindingDesc.type = LLGL::ResourceType::Buffer,
		bindingDesc.bindFlags = LLGL::BindFlags::ConstantBuffer;
		bindingDesc.stageFlags = LLGL::StageFlags::VertexStage;
		bindingDesc.slot = 1;
		LLGL::PipelineLayout* layout = system->CreatePipelineLayout(
			LLGL::PipelineLayoutDescriptor{.heapBindings = {bindingDesc},
										   .bindings = {},
										   .staticSamplers = {},
										   .uniforms = {},
										   .combinedTextureSamplers = {}});

		LLGL::ResourceViewDescriptor viewDesc;
		viewDesc.resource = cbo;
		LLGL::ResourceHeapDescriptor heapDesc;
		heapDesc.pipelineLayout = layout;
		LLGL::ResourceHeap* heap = system->CreateResourceHeap(heapDesc, {&viewDesc, 1});

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

		LLGL::PipelineState* pso = system->CreatePipelineState(LLGL::GraphicsPipelineDescriptor{
			.pipelineLayout = layout,
			.renderPass = ctx.GetSwapChain()->GetRenderPass(),
			.vertexShader = vs,
			.fragmentShader = fs,
			.primitiveTopology = LLGL::PrimitiveTopology::TriangleList,
			.viewports = {},
			.scissors = {},
			.depth = {},
			.stencil = {},
			.rasterizer = {},
			.blend = {},
			.tessellation = {}});

		system->Release(*vs);
		system->Release(*fs);

		// 3. Application Loop
		const JPH::Vec4 background(0.12f, 0.14f, 0.16f, 1.0f);
		float rotation = 0.0f;

		while (ctx.IsRunning()) {
			ctx.ProcessEvents();

			rotation += 0.05f;
			FrameConstants constants{.transform = JPH::Mat44::sRotationZ(rotation)};

			// --- PROCEDURAL RENDER PASS ---
			ctx.BeginFrame();

			Renderer::Clear(ctx, background);
			Renderer::UpdateBuffer(ctx, cbo, constants);
			Renderer::Draw(ctx, pso, heap, vbo, 3);

			ctx.EndFrame();
		}

		// 4. Asset Cleanup
		system->Release(*vbo);
		system->Release(*cbo);
		system->Release(*layout);
		system->Release(*heap);
		system->Release(*pso);
	}

	JPH::UnregisterTypes();
	delete JPH::Factory::sInstance;
	return 0;
}