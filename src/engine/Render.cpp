#include "Allocator.hpp"
#include "DescriptorLayout.hpp"
#include "PipelineBuilder.hpp"
#include "PresentationContext.hpp"
#include "RenderCore.hpp"
#include "RenderGraph.hpp"
#include "SamplerBuilder.hpp"
#include "StandardPasses.hpp"
#include "Texture.hpp"
#include "Vertex.hpp"
#include "Zahlen/Math3D.hpp"

#include <GLFW/glfw3.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <filesystem>
#include <fstream>
#include <unordered_map>

// Map our generic ZHLN::Vertex to the Vulkan Pipeline Builder
ZHLN_REFLECT_VERTEX(ZHLN::Vertex, position, normal, tangent, uv0, uv1);

namespace ZHLN {

using SceneLayout =
	Vk::DescriptorLayout<Vk::BindlessSampledImageSlot<0, 4096>, Vk::SamplerSlot<1>,
						 Vk::SampledImageSlot<2>, Vk::SamplerSlot<3>, Vk::SamplerSlot<4>,
						 Vk::StorageBufferSlot<5, VK_SHADER_STAGE_FRAGMENT_BIT>>;

using FXAALayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, Vk::SamplerSlot<1>>;

// Local helper to load SPIR-V from disk instead of using C++26 #embed
static std::vector<uint32_t> LoadSpirv(const std::string& path) {
	if (!std::filesystem::exists(path))
		return {};
	std::ifstream file(path, std::ios::ate | std::ios::binary);
	if (!file.is_open())
		return {};
	size_t size = static_cast<size_t>(file.tellg());
	std::vector<uint32_t> buffer(size / sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(buffer.data()), size);
	return buffer;
}

struct NativeMesh {
	Vk::Buffer vbo;
	Vk::Buffer ibo;
};

// Groups draw calls by mesh to minimize VBO/IBO re-binding
struct MeshDrawGroup {
	std::vector<Vk::Passes::PBRDrawCall> calls;
};

struct RenderContext::Impl {
	Window& window;
	Vk::Context ctx;
	Vk::Allocator allocator;
	Vk::PresentationContext presentation;

	Vk::FrameSync<2> sync;
	Vk::CommandPools<2> pools;
	uint32_t frame_index = 0;
	bool resized = true;

	Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> sceneColor;
	Vk::RenderTarget<VK_FORMAT_D32_SFLOAT> shadowMap;

	Vk::DescriptorSetLayout globalLayout, fxaaLayout;
	Vk::DescriptorPool globalPool, fxaaPool;
	VkDescriptorSet globalSet, fxaaSet;
	Vk::BindlessRegistry<SceneLayout, 0> bindless;

	Vk::PipelineLayout pbrPipeLayout, shadowPipeLayout, fxaaPipeLayout;
	Vk::Pipeline pbrPipeline, shadowPipeline, fxaaPipeline;
	Vk::Sampler defaultSampler, shadowSampler, lightmapSampler;

	Vk::Buffer lightBuffer;
	std::unordered_map<NativeMesh*, MeshDrawGroup> drawGroups; // Dynamic draw queue
	std::vector<Vk::Passes::Light> lights;

	JPH::Mat44 viewProj;
	JPH::Mat44 shadowProjView;
	JPH::Vec3 camPos;
	JPH::Vec3 sunDir = {0.5f, -1.0f, 0.5f};

	std::vector<std::unique_ptr<NativeMesh>> meshes;
	std::vector<std::unique_ptr<Vk::TextureAsset>> textures;

	uint32_t fallbackTexIdx = 0;

	Impl(Window& win) : window(win) {}
};

RenderContext::RenderContext(Window& window) : _impl(std::make_unique<Impl>(window)) {
	uint32_t glfwExtCount = 0;
	const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
	std::vector<const char*> inst_exts(glfwExts, glfwExts + glfwExtCount);
	inst_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	inst_exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
	inst_exts.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);

#ifdef __APPLE__
	inst_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

	ZHLN_InstanceDesc inst_desc = ZHLN_DEFAULT_INSTANCE_DESC;
	inst_desc.extensions = inst_exts.data();
	inst_desc.extension_count = static_cast<uint32_t>(inst_exts.size());

	// C++ struct defaults fix all compiler warnings about missing designated fields
	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swap_maint{};
	swap_maint.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
	swap_maint.swapchainMaintenance1 = VK_TRUE;

	VkPhysicalDeviceVulkan13Features feat13{};
	feat13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	feat13.pNext = &swap_maint;
	feat13.synchronization2 = VK_TRUE;
	feat13.dynamicRendering = VK_TRUE;

	VkPhysicalDeviceVulkan12Features feat12{};
	feat12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	feat12.pNext = &feat13;
	feat12.descriptorIndexing = VK_TRUE;
	feat12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	feat12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
	feat12.descriptorBindingPartiallyBound = VK_TRUE;
	feat12.runtimeDescriptorArray = VK_TRUE;
	feat12.bufferDeviceAddress = VK_TRUE;

	VkPhysicalDeviceFeatures2 feat2{};
	feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	feat2.pNext = &feat12;
	feat2.features.samplerAnisotropy = VK_TRUE;

#ifdef __APPLE__
	const char* dev_exts[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
		VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME, "VK_KHR_portability_subset"};
#else
	const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
							  VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
							  VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME};
#endif

	ZHLN_DeviceDesc dev_desc = {.physical = nullptr,
								.extensions = dev_exts,
								.extension_count = (uint32_t)std::size(dev_exts),
								.features = &feat2,
								.enable_validation = true};
	_impl->ctx = Vk::Context::Create(inst_desc, {VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr, nullptr},
									 dev_desc);

	VkSurfaceKHR raw_surface;
	glfwCreateWindowSurface(_impl->ctx.Instance(),
							static_cast<GLFWwindow*>(window.GetNativeHandle()), nullptr,
							&raw_surface);
	// 1. Initialize Allocator (Handles VMA)
    if (!_impl->allocator.Init(_impl->ctx)) {
        ZHLN::Panic("FATAL: Failed to initialize Vulkan Memory Allocator. "
                    "This usually indicates the system is out of GPU memory (OOM).");
    }

    // 2. Initialize Presentation Context (Handles Swapchain and Window Depth Buffer)
    // If this fails, it's likely due to an incompatible window size or surface loss.
    if (!_impl->presentation.Init(_impl->ctx, _impl->allocator, raw_surface, 1280, 720)) {
        ZHLN::Panic("FATAL: Failed to initialize Presentation Context. "
                    "Check if the window surface is valid and GPU supports the requested resolution.");
    }

	_impl->sync = Vk::FrameSync<2>::Create(_impl->ctx.Device());
	_impl->pools =
		Vk::CommandPools<2>::Create(_impl->ctx.Device(), _impl->ctx.PhysicalInfo().graphics_family);

	_impl->defaultSampler =
		Vk::SamplerBuilder{}.Linear().Repeat().Anisotropy(8.0f).Build(_impl->ctx.Device());
	_impl->shadowSampler = Vk::SamplerBuilder{}
							   .Linear()
							   .ClampToBorder(VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE)
							   .DepthCompare()
							   .Build(_impl->ctx.Device());
	_impl->lightmapSampler = Vk::SamplerBuilder{}.Linear().ClampToEdge().Build(_impl->ctx.Device());

	_impl->lightBuffer =
		Vk::Buffer::Create(_impl->allocator.Get(), sizeof(Vk::Passes::Light) * 256,
						   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	_impl->shadowMap = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
		_impl->allocator, _impl->ctx, {4096, 4096},
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

	_impl->globalLayout = SceneLayout::CreateLayout(_impl->ctx.Device());
	_impl->globalPool = SceneLayout::CreatePool(_impl->ctx.Device(), 1);
	_impl->globalSet = SceneLayout::Allocate(_impl->ctx.Device(), _impl->globalPool.Get(),
											 _impl->globalLayout.Get());

	SceneLayout::Write(_impl->ctx.Device(), _impl->globalSet, Vk::SkipWrite{},
					   Vk::SamplerWrite{_impl->defaultSampler.Get()},
					   Vk::ImageWrite{_impl->shadowMap.view.Get()},
					   Vk::SamplerWrite{_impl->shadowSampler.Get()},
					   Vk::SamplerWrite{_impl->lightmapSampler.Get()},
					   Vk::BufferWrite{_impl->lightBuffer.Handle()});

	_impl->bindless.Init(_impl->ctx.Device(), _impl->globalSet);

	uint32_t whitePixel = 0xFFFFFFFF;
	_impl->fallbackTexIdx = CreateTexture(&whitePixel, 1, 1);

	_impl->fxaaLayout = FXAALayout::CreateLayout(_impl->ctx.Device());
	_impl->fxaaPool = FXAALayout::CreatePool(_impl->ctx.Device(), 1);
	_impl->fxaaSet =
		FXAALayout::Allocate(_impl->ctx.Device(), _impl->fxaaPool.Get(), _impl->fxaaLayout.Get());

	// Shaders loaded dynamically from disk
	auto pbr_v = LoadSpirv("standard.hlsl.VSMain.spv");
	auto pbr_f = LoadSpirv("standard.hlsl.PSMain.spv");
	auto shd_v = LoadSpirv("standard.hlsl.VSShadow.spv");
	auto shd_f = LoadSpirv("standard.hlsl.PSShadow.spv");
	auto fxa_v = LoadSpirv("fxaa.hlsl.VSMain.spv");
	auto fxa_f = LoadSpirv("fxaa.hlsl.PSMain.spv");

	if (pbr_v.empty() || pbr_f.empty())
		ZHLN::Panic("Missing core shaders! Ensure SPV files exist in the working directory.");

	auto pbrShaders =
		Vk::ShaderStages::Create(_impl->ctx.Device(), {pbr_v.data(), pbr_v.size() * 4, "VSMain"},
								 {pbr_f.data(), pbr_f.size() * 4, "PSMain"});

	auto shdShaders =
		Vk::ShaderStages::Create(_impl->ctx.Device(), {shd_v.data(), shd_v.size() * 4, "VSShadow"},
								 {shd_f.data(), shd_f.size() * 4, "PSShadow"});

	auto fxaShaders =
		Vk::ShaderStages::Create(_impl->ctx.Device(), {fxa_v.data(), fxa_v.size() * 4, "VSMain"},
								 {fxa_f.data(), fxa_f.size() * 4, "PSMain"});

	VkPushConstantRange pbrPush = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
								   sizeof(Vk::Passes::PBRPushConstants)};
	VkPushConstantRange shdPush = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(JPH::Mat44)};
	VkDescriptorSetLayout glbRaw = _impl->globalLayout.Get();
	VkDescriptorSetLayout fxaRaw = _impl->fxaaLayout.Get();

	ZHLN_PipelineLayoutDesc pbrLD = {.set_layouts = &glbRaw,
									 .set_layout_count = 1,
									 .push_constants = &pbrPush,
									 .push_constant_count = 1};
	ZHLN_PipelineLayoutDesc shdLD = {.set_layouts = nullptr,
									 .set_layout_count = 0,
									 .push_constants = &shdPush,
									 .push_constant_count = 1};
	ZHLN_PipelineLayoutDesc fxaLD = {.set_layouts = &fxaRaw,
									 .set_layout_count = 1,
									 .push_constants = nullptr,
									 .push_constant_count = 0};

	_impl->pbrPipeLayout = Vk::PipelineLayout(
		_impl->ctx.Device(), ZHLN_CreatePipelineLayout(_impl->ctx.Device(), &pbrLD));
	_impl->shadowPipeLayout = Vk::PipelineLayout(
		_impl->ctx.Device(), ZHLN_CreatePipelineLayout(_impl->ctx.Device(), &shdLD));
	_impl->fxaaPipeLayout = Vk::PipelineLayout(
		_impl->ctx.Device(), ZHLN_CreatePipelineLayout(_impl->ctx.Device(), &fxaLD));

	_impl->pbrPipeline = Vk::PipelineBuilder{}
							 .Shaders(pbrShaders)
							 .Layout(_impl->pbrPipeLayout.Get())
							 .Vertex<ZHLN::Vertex>()
							 .ColorFormat(VK_FORMAT_R16G16B16A16_SFLOAT)
							 .DepthFormat(VK_FORMAT_D32_SFLOAT)
							 .CullBack()
							 .Build(_impl->ctx.Device());
	_impl->shadowPipeline = Vk::PipelineBuilder{}
								.Shaders(shdShaders)
								.Layout(_impl->shadowPipeLayout.Get())
								.Vertex<ZHLN::Vertex>()
								.DepthOnly()
								.CullNone()
								.Build(_impl->ctx.Device());
	_impl->fxaaPipeline = Vk::PipelineBuilder{}
							  .Shaders(fxaShaders)
							  .Layout(_impl->fxaaPipeLayout.Get())
							  .ColorFormat(VK_FORMAT_B8G8R8A8_SRGB)
							  .NoDepth()
							  .CullNone()
							  .Build(_impl->ctx.Device());
}

RenderContext::~RenderContext() {
	if (_impl->ctx.Device())
		vkDeviceWaitIdle(_impl->ctx.Device());
}

const char* RenderContext::GetRendererName() const {
	return "ZHLN Bindless PBR Graph (Vulkan 1.3)";
}
void RenderContext::SetResolution(const Extent2D&) {
	_impl->resized = true;
}

void RenderContext::SetCamera(const JPH::Mat44& viewProj, const JPH::Vec3& camPos) {
	_impl->viewProj = viewProj;
	_impl->camPos = camPos;
}

void RenderContext::SetSunlight(const JPH::Vec3& dir, const JPH::Vec3& color, float intensity) {
	_impl->sunDir = dir.Normalized();
	_impl->lights.clear();
	_impl->lights.push_back(
		{.position = {0, 0, 0},
		 .type = 0, // 0 = Directional
		 .color = {color.GetX(), color.GetY(), color.GetZ()},
		 .intensity = intensity,
		 .direction = {_impl->sunDir.GetX(), _impl->sunDir.GetY(), _impl->sunDir.GetZ()},
		 .range = 0,
		 .innerConeCos = 0,
		 .outerConeCos = 0});
}

uint32_t RenderContext::CreateTexture(const void* pixels, uint32_t width, uint32_t height) {
	VkImageCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.extent = {width, height, 1};

	auto tex = std::make_unique<Vk::TextureAsset>(
		Vk::UploadTexture<VK_FORMAT_R8G8B8A8_UNORM>(_impl->allocator, _impl->ctx, info, pixels));
	uint32_t idx = _impl->bindless.RegisterImage(tex->view.Get());
	_impl->textures.push_back(std::move(tex));
	return idx;
}

Material RenderContext::CreateMaterial() {
	return Material{.albedoIdx = _impl->fallbackTexIdx,
					.normalIdx = _impl->fallbackTexIdx,
					.pbrIdx = _impl->fallbackTexIdx,
					.emissiveIdx = _impl->fallbackTexIdx,
					.lightmapIdx = _impl->fallbackTexIdx};
}

Mesh RenderContext::CreateMesh(const Vertex* vertices, size_t vertexCount, const uint32_t* indices,
							   size_t indexCount) {
	VkCommandBuffer cmd = _impl->pools.Cmd(0);
	ZHLN_BeginCommandBuffer(cmd);

	auto vbo =
		Vk::Buffer::Create(_impl->allocator.Get(), vertexCount * sizeof(Vertex),
						   VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
						   VMA_MEMORY_USAGE_GPU_ONLY);
	auto ibo =
		Vk::Buffer::Create(_impl->allocator.Get(), indexCount * sizeof(uint32_t),
						   VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
						   VMA_MEMORY_USAGE_GPU_ONLY);

	auto stV = Vk::UploadToBuffer(_impl->allocator.Get(), cmd, vbo, vertices,
								  vertexCount * sizeof(Vertex));
	auto stI = Vk::UploadToBuffer(_impl->allocator.Get(), cmd, ibo, indices,
								  indexCount * sizeof(uint32_t));

	ZHLN_EndCommandBuffer(cmd);
	VkCommandBufferSubmitInfo cmdInfo{};
	cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
	cmdInfo.commandBuffer = cmd;
	VkSubmitInfo2 submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
	submit.commandBufferInfoCount = 1;
	submit.pCommandBufferInfos = &cmdInfo;
	vkQueueSubmit2(_impl->ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(_impl->ctx.GraphicsQueue());

	auto* mesh = new NativeMesh{std::move(vbo), std::move(ibo)};
	_impl->meshes.emplace_back(mesh);

	return Mesh{.vertexBuffer = static_cast<BufferHandle>(reinterpret_cast<uint64_t>(mesh)),
				.indexBuffer = static_cast<BufferHandle>(reinterpret_cast<uint64_t>(mesh)),
				.indexCount = static_cast<uint32_t>(indexCount),
				.firstIndex = 0};
}

void RenderContext::BeginFrame() {
	if (_impl->resized) {
		int w, h;
		glfwGetFramebufferSize(static_cast<GLFWwindow*>(_impl->window.GetNativeHandle()), &w, &h);
		_impl->presentation.Rebuild(w, h);

		_impl->sceneColor = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, _impl->presentation.swapchain.Get().extent,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

		FXAALayout::Write(_impl->ctx.Device(), _impl->fxaaSet,
						  Vk::ImageWrite{_impl->sceneColor.view.Get()},
						  Vk::SamplerWrite{_impl->defaultSampler.Get()});
		_impl->resized = false;
	}
	_impl->drawGroups.clear();
}

void RenderContext::EndFrame() {
	if (!_impl->presentation.swapchain.Valid() ||
		_impl->presentation.swapchain.Get().extent.width == 0)
		return;

	if (auto m = _impl->lightBuffer.Map(); m.data) {
		memcpy(m.data, _impl->lights.data(), _impl->lights.size() * sizeof(Vk::Passes::Light));
	}

	JPH::Vec3 shadowTarget = _impl->camPos - (_impl->sunDir * 20.0f);
	JPH::Mat44 lightView =
		Math::CreateLookAt(shadowTarget + (_impl->sunDir * 100.0f), shadowTarget, {0, 1, 0});
	JPH::Mat44 lightProj = Math::CreateOrtho(-50, 50, -50, 50, 0.1f, 200.0f);
	_impl->shadowProjView = lightProj * lightView;

	JPH::Mat44 biasMatrix =
		JPH::Mat44(JPH::Vec4(0.5f, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, 0.5f, 0.0f, 0.0f),
				   JPH::Vec4(0.0f, 0.0f, 1.0f, 0.0f), JPH::Vec4(0.5f, 0.5f, 0.0f, 1.0f));

	Vk::Passes::PBRSceneContext sceneCtx = {
		.viewProj = {_impl->viewProj.GetColumn4(0).mF32[0], _impl->viewProj.GetColumn4(0).mF32[1],
					 _impl->viewProj.GetColumn4(0).mF32[2], _impl->viewProj.GetColumn4(0).mF32[3],
					 _impl->viewProj.GetColumn4(1).mF32[0], _impl->viewProj.GetColumn4(1).mF32[1],
					 _impl->viewProj.GetColumn4(1).mF32[2], _impl->viewProj.GetColumn4(1).mF32[3],
					 _impl->viewProj.GetColumn4(2).mF32[0], _impl->viewProj.GetColumn4(2).mF32[1],
					 _impl->viewProj.GetColumn4(2).mF32[2], _impl->viewProj.GetColumn4(2).mF32[3],
					 _impl->viewProj.GetColumn4(3).mF32[0], _impl->viewProj.GetColumn4(3).mF32[1],
					 _impl->viewProj.GetColumn4(3).mF32[2], _impl->viewProj.GetColumn4(3).mF32[3]},
		.lightSpaceMatrix = {},
		.shadowProjView = {},
		.camPos = {_impl->camPos.GetX(), _impl->camPos.GetY(), _impl->camPos.GetZ(), 1.0f},
		.lightDir = {_impl->sunDir.GetX(), _impl->sunDir.GetY(), _impl->sunDir.GetZ(), 0.0f},
		.lightCount = (uint32_t)_impl->lights.size(),
		.globalSet = _impl->globalSet,
		.vbo = VK_NULL_HANDLE,
		.ibo = VK_NULL_HANDLE};

	JPH::Mat44 biased = biasMatrix * _impl->shadowProjView;
	std::memcpy(sceneCtx.lightSpaceMatrix.data(), &biased, 64);
	std::memcpy(sceneCtx.shadowProjView.data(), &_impl->shadowProjView, 64);

	auto record_cb = [&](VkCommandBuffer cmd, uint32_t image_index) {
		Vk::GraphImage swapRes = Vk::GraphImage::Create(
			_impl->presentation.swapchain.Get().images[image_index],
			_impl->presentation.swapchain.Get().views[image_index],
			_impl->presentation.swapchain.Get().extent, VK_IMAGE_ASPECT_COLOR_BIT);

		// 1. Create a context struct to pass into the lambdas
		struct PassContext {
			RenderContext::Impl* impl;
			const Vk::Passes::PBRSceneContext* scene;
		} pCtx = {_impl.get(), &sceneCtx};

		Vk::RenderGraph graph;

		// SHADOW PASS
		graph.AddPass("Shadows")
			.WriteDepth(_impl->shadowMap.tracker)
			.Record(
				[](VkCommandBuffer c, const void* data) { // Capture list MUST be empty []
					auto* ctx = static_cast<const PassContext*>(data);
					vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS,
									  ctx->impl->shadowPipeline.Get());

					for (auto& [msh, group] : ctx->impl->drawGroups) {
						VkBuffer vbo = msh->vbo.Handle();
						VkDeviceSize offset = 0;
						vkCmdBindVertexBuffers(c, 0, 1, &vbo, &offset);
						vkCmdBindIndexBuffer(c, msh->ibo.Handle(), 0, VK_INDEX_TYPE_UINT32);

						for (auto& call : group.calls) {
							Vk::Passes::ShadowPushConstants pc = {
								.mvp = Vk::Passes::Multiply(ctx->scene->shadowProjView,
															call.worldMatrix)};
							Vk::Push(c, ctx->impl->shadowPipeLayout.Get(),
									 VK_SHADER_STAGE_VERTEX_BIT, pc);
							vkCmdDrawIndexed(c, call.indexCount, 1, call.firstIndex, 0, 0);
						}
					}
				},
				&pCtx); // Pass the struct pointer here

		// PBR MAIN PASS
		graph.AddPass("PBR")
			.Read(_impl->shadowMap.tracker)
			.WriteColor(_impl->sceneColor.tracker)
			.WriteDepth(_impl->presentation.depthTarget.tracker)
			.Record(
				[](VkCommandBuffer c, const void* data) { // Capture list MUST be empty []
					auto* ctx = static_cast<const PassContext*>(data);
					vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS,
									  ctx->impl->pbrPipeline.Get());
					vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS,
											ctx->impl->pbrPipeLayout.Get(), 0, 1,
											&ctx->scene->globalSet, 0, nullptr);

					for (auto& [msh, group] : ctx->impl->drawGroups) {
						VkBuffer vbo = msh->vbo.Handle();
						VkDeviceSize offset = 0;
						vkCmdBindVertexBuffers(c, 0, 1, &vbo, &offset);
						vkCmdBindIndexBuffer(c, msh->ibo.Handle(), 0, VK_INDEX_TYPE_UINT32);

						for (auto& call : group.calls) {
							Vk::Passes::PBRPushConstants pc = {
								.mvp = Vk::Passes::Multiply(ctx->scene->viewProj, call.worldMatrix),
								.lightSpaceMatrix = Vk::Passes::Multiply(
									ctx->scene->lightSpaceMatrix, call.worldMatrix),
								.worldMatrix = call.worldMatrix,
								.camPos = {ctx->scene->camPos[0], ctx->scene->camPos[1],
										   ctx->scene->camPos[2], 1.0f},
								.lightDir = {ctx->scene->lightDir[0], ctx->scene->lightDir[1],
											 ctx->scene->lightDir[2], 0.0f},
								.albedoIdx = call.albedoIdx,
								.normalIdx = call.normalIdx,
								.pbrIdx = call.pbrIdx,
								.lightmapIdx = call.lightmapIdx,
								.emissiveIdx = call.emissiveIdx,
								.lightCount = ctx->scene->lightCount};
							Vk::Push(c, ctx->impl->pbrPipeLayout.Get(),
									 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, pc);
							vkCmdDrawIndexed(c, call.indexCount, 1, call.firstIndex, 0, 0);
						}
					}
				},
				&pCtx);

		// FXAA PASS
		graph.AddPass("FXAA")
			.Read(_impl->sceneColor.tracker)
			.WriteColor(swapRes)
			.Record(
				[](VkCommandBuffer c, const void* data) { // Capture list MUST be empty []
					auto* ctx = static_cast<const PassContext*>(data);
					vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS,
									  ctx->impl->fxaaPipeline.Get());
					vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS,
											ctx->impl->fxaaPipeLayout.Get(), 0, 1,
											&ctx->impl->fxaaSet, 0, nullptr);
					vkCmdDraw(c, 3, 1, 0, 0);
				},
				&pCtx);

		graph.AddPass("Present").Present(swapRes);
		graph.Execute(cmd);
	};

	auto result =
		Vk::DrawFrame(_impl->ctx, _impl->presentation.swapchain, _impl->sync, _impl->pools,
					  _impl->frame_index, record_cb, [&]() { _impl->resized = true; });
	if (result != ZHLN_FrameResult_Ok)
		_impl->resized = true;
}

namespace Renderer {
void Draw(RenderContext& ctx, const Material& mat, const Mesh& mesh, const JPH::Mat44& tf) {
	auto* msh = reinterpret_cast<NativeMesh*>(static_cast<uint64_t>(mesh.vertexBuffer));

	std::array<float, 16> matData;
	std::memcpy(matData.data(), &tf, 64);

	// Group the call by the mesh pointer, minimizing Vulkan binds
	ctx.GetImpl()->drawGroups[msh].calls.push_back({.worldMatrix = matData,
													.albedoIdx = mat.albedoIdx,
													.normalIdx = mat.normalIdx,
													.pbrIdx = mat.pbrIdx,
													.lightmapIdx = mat.lightmapIdx,
													.emissiveIdx = mat.emissiveIdx,
													.indexCount = mesh.indexCount,
													.firstIndex = mesh.firstIndex});
}
} // namespace Renderer

} // namespace ZHLN