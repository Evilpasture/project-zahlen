#include "../cube_app/math.hpp"
#include "Allocator.hpp"
#include "DescriptorLayout.hpp"
#include "PipelineBuilder.hpp"
#include "RenderCore.hpp"
#include "SamplerBuilder.hpp" // NEW
#include "Vertex.hpp"
#include "demo_utils/DemoWindow.hpp"
// clang-format off
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
// clang-format on
#include <array>
#include <chrono>
#include <filesystem>
#include <print>
#include <vector>

ZHLN_REFLECT_VERTEX(ZHLN::Vk::Vertex, pos, norm, uv);

// ============================================================================
// Types
// ============================================================================

struct MeshPrimitive {
	uint32_t indexCount;
	uint32_t firstIndex;
	int materialIndex;
};

struct DrawCall {
	MeshPrimitive* mesh;
	Mat4 worldMatrix;
};

struct TextureAsset {
	ZHLN::Vk::Image image;
	ZHLN::Vk::ImageView view;
};

struct MaterialAsset {
	VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
};

struct SponzaPushConstants {
	Mat4 mvp;
	Mat4 lightSpaceMatrix;
	float camPos[4];
};

// Material layout — single source of truth for all 4 bindings
using MaterialLayout = ZHLN::Vk::DescriptorLayout<ZHLN::Vk::SampledImageSlot<0>, // base color
												  ZHLN::Vk::SamplerSlot<1>,		 // default sampler
												  ZHLN::Vk::SampledImageSlot<2>, // shadow map
												  ZHLN::Vk::SamplerSlot<3>		 // shadow sampler
												  >;

// ============================================================================
// Helpers
// ============================================================================

[[nodiscard]] static bool FileExists(const std::string& path) {
	bool exists = std::filesystem::exists(path);
	if (!exists)
		std::println(stderr, "ERROR: File not found: {}", std::filesystem::absolute(path).string());
	return exists;
}

// ============================================================================
// Path resolution
// ============================================================================

struct AssetPaths {
	std::string asset_prefix;
	std::string shader_prefix;
	std::string gltf;
	std::string vert_spv;
	std::string frag_spv;
	std::string shadow_vert_spv;
	std::string shadow_frag_spv;

	[[nodiscard]] bool Validate() const {
		return FileExists(gltf) && FileExists(vert_spv) && FileExists(frag_spv) &&
			   FileExists(shadow_vert_spv) && FileExists(shadow_frag_spv);
	}
};

static AssetPaths ResolvePaths() {
	AssetPaths p;
	p.asset_prefix = "resources/assets/main_sponza/";
	p.shader_prefix = "";

	if (std::filesystem::exists("build/src/render/sponza.hlsl.VSMain.spv")) {
		p.shader_prefix = "build/src/render/";
	} else if (std::filesystem::exists("../../../resources")) {
		p.asset_prefix = "../../../resources/assets/main_sponza/";
		p.shader_prefix = "./";
	} else {
		p.shader_prefix = "./";
	}

	p.gltf = p.asset_prefix + "NewSponza_Main_glTF_003.gltf";
	p.vert_spv = p.shader_prefix + "sponza.hlsl.VSMain.spv";
	p.frag_spv = p.shader_prefix + "sponza.hlsl.PSMain.spv";
	p.shadow_vert_spv = p.shader_prefix + "shadow.hlsl.VSMain.spv";
	p.shadow_frag_spv = p.shader_prefix + "shadow.hlsl.PSMain.spv";
	return p;
}

// ============================================================================
// Scene loading
// ============================================================================

struct Scene {
	std::vector<ZHLN::Vk::Vertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<MeshPrimitive> primitives; // stable storage
	std::vector<DrawCall> drawCalls;	   // pointers into primitives
};

static Scene BuildScene(cgltf_data* data) {
	Scene scene;
	std::vector<std::vector<size_t>> meshPrimIndices(data->meshes_count);

	for (cgltf_size i = 0; i < data->meshes_count; ++i) {
		for (cgltf_size j = 0; j < data->meshes[i].primitives_count; ++j) {
			cgltf_primitive* prim = &data->meshes[i].primitives[j];

			uint32_t firstIndex = static_cast<uint32_t>(scene.indices.size());
			uint32_t vertexOffset = static_cast<uint32_t>(scene.vertices.size());

			for (cgltf_size k = 0; k < prim->indices->count; ++k)
				scene.indices.push_back(cgltf_accessor_read_index(prim->indices, k) + vertexOffset);

			size_t vertexCount = prim->attributes[0].data->count;
			size_t startVert = scene.vertices.size();
			scene.vertices.resize(scene.vertices.size() + vertexCount);

			for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
				cgltf_attribute* attr = &prim->attributes[a];
				for (cgltf_size v = 0; v < vertexCount; ++v) {
					if (attr->type == cgltf_attribute_type_position)
						cgltf_accessor_read_float(attr->data, v,
												  scene.vertices[startVert + v].pos.data(), 3);
					else if (attr->type == cgltf_attribute_type_normal)
						cgltf_accessor_read_float(attr->data, v,
												  scene.vertices[startVert + v].norm.data(), 3);
					else if (attr->type == cgltf_attribute_type_texcoord)
						cgltf_accessor_read_float(attr->data, v,
												  scene.vertices[startVert + v].uv.data(), 2);
				}
			}

			meshPrimIndices[i].push_back(scene.primitives.size());
			scene.primitives.push_back({
				.indexCount = static_cast<uint32_t>(prim->indices->count),
				.firstIndex = firstIndex,
				.materialIndex =
					prim->material ? static_cast<int>(prim->material - data->materials) : -1,
			});
		}
	}

	// Walk nodes — primitives vector won't reallocate anymore
	for (cgltf_size i = 0; i < data->nodes_count; ++i) {
		cgltf_node* node = &data->nodes[i];
		if (!node->mesh)
			continue;

		float matrix[16];
		cgltf_node_transform_world(node, matrix);
		Mat4 worldMat;
		std::copy(matrix, matrix + 16, worldMat.data.begin());

		size_t meshIdx = node->mesh - data->meshes;
		for (size_t primIdx : meshPrimIndices[meshIdx])
			scene.drawCalls.push_back({&scene.primitives[primIdx], worldMat});
	}

	return scene;
}

// ============================================================================
// Texture upload
// ============================================================================

static TextureAsset UploadTexture(ZHLN::Vk::Allocator& allocator, ZHLN::Demo::WindowState& win,
								  const ZHLN::Vk::Context& ctx, const VkImageCreateInfo& baseInfo,
								  const std::string& fullPath) {
	TextureAsset result;
	int tw, th, tc;
	stbi_uc* pixels = stbi_load(fullPath.c_str(), &tw, &th, &tc, STBI_rgb_alpha);
	if (!pixels)
		return result;

	ZHLN::Vk::CommandPool batchPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	if (!batchPool.Allocate(1)) {
		stbi_image_free(pixels);
		return result;
	}
	VkCommandBuffer cmd = batchPool[0];
	ZHLN_BeginCommandBuffer(cmd);

	VkImageCreateInfo texInfo = baseInfo;
	texInfo.extent = {static_cast<uint32_t>(tw), static_cast<uint32_t>(th), 1};
	result.image = ZHLN::Vk::Image::Create(allocator.Get(), texInfo, VMA_MEMORY_USAGE_GPU_ONLY);

	size_t imageSize = tw * th * 4;
	auto staging = ZHLN::Vk::Buffer::Create(
		allocator.Get(), imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	memcpy(staging.Map().data, pixels, imageSize);

	ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
		cmd, result.image.Handle());
	ZHLN::Vk::CopyBufferToImage(cmd, {.buffer = staging.Handle(),
									  .image = result.image.Handle(),
									  .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
									  .width = (uint32_t)tw,
									  .height = (uint32_t)th});
	ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd,
																		 result.image.Handle());
	ZHLN_EndCommandBuffer(cmd);

	VkCommandBufferSubmitInfo subInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
										 .commandBuffer = cmd};
	VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
							.commandBufferInfoCount = 1,
							.pCommandBufferInfos = &subInfo};
	vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(ctx.GraphicsQueue());

	stbi_image_free(pixels);
	// staging and batchPool destroyed here

	result.view =
		ZHLN::Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(ctx.Device(), result.image.Handle());
	return result;
}

// ============================================================================
// FPS counter
// ============================================================================

struct FpsCounter {
	std::chrono::high_resolution_clock::time_point lastUpdate =
		std::chrono::high_resolution_clock::now();
	uint32_t frames = 0;

	// Returns a formatted title string once per second, empty otherwise
	std::string Tick(size_t drawCallCount) {
		++frames;
		auto now = std::chrono::high_resolution_clock::now();
		float elapsed = std::chrono::duration<float>(now - lastUpdate).count();
		if (elapsed < 1.0f)
			return {};

		float fps = static_cast<float>(frames) / elapsed;
		frames = 0;
		lastUpdate = now;
		return std::format("ZHLN Engine | FPS: {:.1f} ({:.2f} ms) | Draw calls: {}", fps,
						   1000.0f / fps, drawCallCount);
	}
};

// ============================================================================
// Swapchain rebuild
// ============================================================================

struct FrameResources {
	ZHLN::Vk::Swapchain swapchain;
	ZHLN::Vk::SemaphorePool presentSemaphores;
	ZHLN::Vk::Image depthImage;
	ZHLN::Vk::ImageView depthView;
	bool depthInitialized = false;

	void Rebuild(const ZHLN::Vk::Context& ctx, ZHLN::Vk::Allocator& allocator,
				 ZHLN::Demo::WindowState& win, VkSurfaceKHR surface) {
		vkDeviceWaitIdle(ctx.Device());
		depthInitialized = false;

		ZHLN_Device rawDev = {ctx.Device(), ctx.GraphicsQueue(), ctx.PresentQueue()};
		ZHLN_PhysicalDeviceInfo rawPhys = ctx.PhysicalInfo();
		ZHLN_SwapchainDesc desc = {.device = &rawDev,
								   .physical = &rawPhys,
								   .surface = surface,
								   .width = win.width,
								   .height = win.height,
								   .vsync = true,
								   .old_swapchain = VK_NULL_HANDLE};
		swapchain.Rebuild(desc);
		presentSemaphores.Rebuild(ctx.Device(), swapchain.Get().image_count);

		VkImageCreateInfo depthInfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = VK_FORMAT_D32_SFLOAT,
			.extent = {win.width, win.height, 1},
			.mipLevels = 1,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		depthImage = ZHLN::Vk::Image::Create(allocator.Get(), depthInfo, VMA_MEMORY_USAGE_GPU_ONLY);
		depthView = ZHLN::Vk::CreateView<VK_FORMAT_D32_SFLOAT>(ctx.Device(), depthImage.Handle());

		win.resized = false;
	}
};

// ============================================================================
// Frame recording
// ============================================================================

struct PipelineSet {
	VkPipeline pipeline, shadowPipeline;
	VkPipelineLayout pipelineLayout, shadowLayout;
};

struct SceneBuffers {
	VkBuffer vbo, ibo;
	const std::vector<DrawCall>& drawCalls;
	const std::vector<MaterialAsset>& materials;
	const MaterialAsset& fallbackMat;
};

struct FrameRecordDesc {
	VkCommandBuffer cmd;

	// Dynamic Frame Targets
	VkImage swapchainImage;
	VkImageView swapchainView;
	VkExtent2D extent;

	VkImage depthImage;
	VkImageView depthView;
	bool& depthInitialized;

	VkImage shadowImage;
	VkImageView shadowView;

	// State
	const PipelineSet& pipelines;
	const SceneBuffers& scene;

	// Transforms
	Mat4 viewProj;
	Mat4 lightSpaceMatrix;
	float camPos[3];
};

static void RecordFrame(const FrameRecordDesc& d) {
	// Swapchain image → color attachment
	ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
		d.cmd, d.swapchainImage);

	// Depth image — one-time transition on first use
	if (!d.depthInitialized) {
		ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED,
								   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
			d.cmd, d.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT);
		d.depthInitialized = true;
	}

	VkBuffer vboHandle = d.scene.vbo;
	VkDeviceSize offset = 0;

	// --- Pass 1: shadow ---
	ZHLN_ImageBarrierDesc shadowWriteBarrier = {
		.image = d.shadowImage,
		.src_access = VK_ACCESS_2_NONE,
		.dst_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		.src_layout = VK_IMAGE_LAYOUT_UNDEFINED,
		.dst_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
		.src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
		.dst_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
		.aspect = VK_IMAGE_ASPECT_DEPTH_BIT};
	ZHLN_CmdImageBarrier(d.cmd, &shadowWriteBarrier);

	{
		ZHLN_RenderPassDesc shadowPass = {.target_view = VK_NULL_HANDLE,
										  .depth_view = d.shadowView,
										  .extent = {4096, 4096},
										  .clear_depth = 1.0f};
		ZHLN::Vk::ScopedRendering render(d.cmd, shadowPass);
		vkCmdBindPipeline(d.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipelines.shadowPipeline);
		vkCmdBindVertexBuffers(d.cmd, 0, 1, &vboHandle, &offset);
		vkCmdBindIndexBuffer(d.cmd, d.scene.ibo, 0, VK_INDEX_TYPE_UINT32);

		for (const auto& draw : d.scene.drawCalls) {
			Mat4 shadowMVP = Multiply(d.lightSpaceMatrix, draw.worldMatrix);
			ZHLN::Vk::Push(d.cmd, d.pipelines.shadowLayout, VK_SHADER_STAGE_VERTEX_BIT, shadowMVP);
			vkCmdDrawIndexed(d.cmd, draw.mesh->indexCount, 1, draw.mesh->firstIndex, 0, 0);
		}
	}

	// Shadow map → shader read
	ZHLN_ImageBarrierDesc shadowReadBarrier = {
		.image = d.shadowImage,
		.src_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		.dst_access = VK_ACCESS_2_SHADER_READ_BIT,
		.src_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
		.dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.src_stage = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
		.dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
		.aspect = VK_IMAGE_ASPECT_DEPTH_BIT};
	ZHLN_CmdImageBarrier(d.cmd, &shadowReadBarrier);

	// --- Pass 2: main scene ---
	{
		ZHLN_RenderPassDesc mainPass = {.target_view = d.swapchainView,
										.depth_view = d.depthView,
										.extent = d.extent,
										.clear_color = {0.5f, 0.7f, 1.0f, 1.0f},
										.clear_depth = 1.0f};
		ZHLN::Vk::ScopedRendering render(d.cmd, mainPass);
		vkCmdBindPipeline(d.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipelines.pipeline);
		vkCmdBindVertexBuffers(d.cmd, 0, 1, &vboHandle, &offset);
		vkCmdBindIndexBuffer(d.cmd, d.scene.ibo, 0, VK_INDEX_TYPE_UINT32);

		int current_mat = -2;
		for (const auto& draw : d.scene.drawCalls) {
			SponzaPushConstants pc = {
				.mvp = Multiply(d.viewProj, draw.worldMatrix),
				.lightSpaceMatrix = Multiply(d.lightSpaceMatrix, draw.worldMatrix),
				.camPos = {d.camPos[0], d.camPos[1], d.camPos[2], 1.0f},
			};
			ZHLN::Vk::Push(d.cmd, d.pipelines.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, pc);

			if (draw.mesh->materialIndex != current_mat) {
				current_mat = draw.mesh->materialIndex;
				VkDescriptorSet dSet = (current_mat >= 0)
										   ? d.scene.materials[current_mat].descriptorSet
										   : d.scene.fallbackMat.descriptorSet;
				vkCmdBindDescriptorSets(d.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
										d.pipelines.pipelineLayout, 0, 1, &dSet, 0, nullptr);
			}
			vkCmdDrawIndexed(d.cmd, draw.mesh->indexCount, 1, draw.mesh->firstIndex, 0, 0);
		}
	}

	// Transition to PRESENT_SRC_KHR before handing off
	ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(d.cmd, d.swapchainImage);
}

// ============================================================================
// main
// ============================================================================

auto main() -> int {
	const AssetPaths paths = ResolvePaths();

	ZHLN::Demo::WindowState win = ZHLN::Demo::InitWindow(1280, 720, "ZHLN Engine - Sponza Atrium");

	// --- Vulkan context ---
	ZHLN_InstanceDesc inst_desc = ZHLN_VERBOSE_INSTANCE_DESC;
	auto required_exts = ZHLN::Demo::GetRequiredInstanceExtensions();
	required_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	required_exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
	required_exts.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
#ifdef __APPLE__
	required_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif
	inst_desc.extensions = required_exts.data();
	inst_desc.extension_count = static_cast<uint32_t>(required_exts.size());

	VkPhysicalDeviceVulkan13Features feat13 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.synchronization2 = VK_TRUE,
		.dynamicRendering = VK_TRUE,
		.shaderDemoteToHelperInvocation = VK_TRUE};
	VkPhysicalDeviceVulkan12Features feat12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &feat13,
		.bufferDeviceAddress = VK_TRUE};
	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swap_maint = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
		.swapchainMaintenance1 = VK_TRUE};
	feat13.pNext = &swap_maint;
	VkPhysicalDeviceFeatures2 feat2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
									   .pNext = &feat12,
									   .features = {.samplerAnisotropy = VK_TRUE}};

#ifdef __APPLE__
	const char* dev_exts[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
		VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME, "VK_KHR_portability_subset"};
	const uint32_t dev_ext_count = 4;
#else
	const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
							  VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
							  VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME};
	const uint32_t dev_ext_count = 3;
#endif

	ZHLN_DeviceDesc dev_desc = {.extensions = dev_exts,
								.extension_count = dev_ext_count,
								.features = &feat2,
								.enable_validation = true};
	auto ctx = ZHLN::Vk::Context::Create(inst_desc, {VK_NULL_HANDLE, VK_NULL_HANDLE}, dev_desc);
	if (!ctx)
		return -1;

	VkSurfaceKHR raw_surface = ZHLN::Demo::CreateSurface(ctx.Instance(), win);
	ZHLN::Vk::Surface surface(ctx.Instance(), raw_surface);

	ZHLN::Vk::Allocator allocator;
	if (!allocator.Init(ctx))
		return -1;

	if (!paths.Validate())
		return -1;

	// --- Load glTF ---
	std::println("Loading Sponza...");
	cgltf_options options = {};
	cgltf_data* data = nullptr;
	if (cgltf_parse_file(&options, paths.gltf.c_str(), &data) != cgltf_result_success ||
		cgltf_load_buffers(&options, data, paths.gltf.c_str()) != cgltf_result_success) {
		std::println(stderr, "FATAL: Failed to load glTF.");
		return -1;
	}

	Scene scene = BuildScene(data);

	// --- GPU buffers ---
	ZHLN::Vk::CommandPool setupPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	if (!setupPool.Allocate(1))
		return -1;
	VkCommandBuffer setupCmd = setupPool[0];
	ZHLN_BeginCommandBuffer(setupCmd);

	auto vbo = ZHLN::Vk::Buffer::Create(
		allocator.Get(), scene.vertices.size() * sizeof(ZHLN::Vk::Vertex),
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY);
	auto ibo = ZHLN::Vk::Buffer::Create(allocator.Get(), scene.indices.size() * sizeof(uint32_t),
										VK_BUFFER_USAGE_TRANSFER_DST_BIT |
											VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
										VMA_MEMORY_USAGE_GPU_ONLY);

	auto stagingVBO =
		ZHLN::Vk::UploadToBuffer(allocator.Get(), setupCmd, vbo, scene.vertices.data(),
								 scene.vertices.size() * sizeof(ZHLN::Vk::Vertex));
	auto stagingIBO = ZHLN::Vk::UploadToBuffer(allocator.Get(), setupCmd, ibo, scene.indices.data(),
											   scene.indices.size() * sizeof(uint32_t));

	// Dummy 1x1 white texture
	uint32_t white_pixel = 0xFFFFFFFF;
	VkImageCreateInfo texBaseInfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
									 .imageType = VK_IMAGE_TYPE_2D,
									 .format = VK_FORMAT_R8G8B8A8_UNORM,
									 .extent = {1, 1, 1},
									 .mipLevels = 1,
									 .arrayLayers = 1,
									 .samples = VK_SAMPLE_COUNT_1_BIT,
									 .tiling = VK_IMAGE_TILING_OPTIMAL,
									 .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
											  VK_IMAGE_USAGE_SAMPLED_BIT,
									 .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
									 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};

	TextureAsset dummyTex;
	dummyTex.image =
		ZHLN::Vk::Image::Create(allocator.Get(), texBaseInfo, VMA_MEMORY_USAGE_GPU_ONLY);
	auto stagingDummy = ZHLN::Vk::Buffer::Create(
		allocator.Get(), 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	memcpy(stagingDummy.Map().data, &white_pixel, 4);

	ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
		setupCmd, dummyTex.image.Handle());
	ZHLN::Vk::CopyBufferToImage(setupCmd, {.buffer = stagingDummy.Handle(),
										   .image = dummyTex.image.Handle(),
										   .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
										   .width = 1,
										   .height = 1});
	ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(setupCmd,
																		 dummyTex.image.Handle());

	ZHLN_EndCommandBuffer(setupCmd);
	VkCommandBufferSubmitInfo setupCmdInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
											  .commandBuffer = setupCmd};
	VkSubmitInfo2 setupSubmit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
								 .commandBufferInfoCount = 1,
								 .pCommandBufferInfos = &setupCmdInfo};
	vkQueueSubmit2(ctx.GraphicsQueue(), 1, &setupSubmit, VK_NULL_HANDLE);
	vkQueueWaitIdle(ctx.GraphicsQueue());

	dummyTex.view =
		ZHLN::Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(ctx.Device(), dummyTex.image.Handle());

	// --- Textures Loop ---
	std::vector<TextureAsset> textures(data->images_count);
	std::println("Uploading {} textures...", data->images_count);
	for (cgltf_size i = 0; i < data->images_count; ++i) {
		ZHLN::Demo::ProcessEvents(win);
		std::string fullPath = paths.asset_prefix + data->images[i].uri;
		textures[i] = UploadTexture(allocator, win, ctx, texBaseInfo, fullPath);
		std::println("  [{}/{}] {}", i + 1, data->images_count, data->images[i].uri);
	}

	// --- Shadow map ---
	constexpr uint32_t SHADOW_RES = 4096;
	VkImageCreateInfo shadowInfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
									.imageType = VK_IMAGE_TYPE_2D,
									.format = VK_FORMAT_D32_SFLOAT,
									.extent = {SHADOW_RES, SHADOW_RES, 1},
									.mipLevels = 1,
									.arrayLayers = 1,
									.samples = VK_SAMPLE_COUNT_1_BIT,
									.tiling = VK_IMAGE_TILING_OPTIMAL,
									.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
											 VK_IMAGE_USAGE_SAMPLED_BIT,
									.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
									.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	auto shadowImage =
		ZHLN::Vk::Image::Create(allocator.Get(), shadowInfo, VMA_MEMORY_USAGE_GPU_ONLY);
	auto shadowView =
		ZHLN::Vk::CreateView<VK_FORMAT_D32_SFLOAT>(ctx.Device(), shadowImage.Handle());

	// --- Samplers (Via Builder) ---
	auto defaultSampler =
		ZHLN::Vk::SamplerBuilder{}.Linear().Repeat().Anisotropy(8.0f).Build(ctx.Device());
	auto shadowSampler = ZHLN::Vk::SamplerBuilder{}
							 .Linear()
							 .ClampToBorder(VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE)
							 .DepthCompare()
							 .Build(ctx.Device());

	// --- Descriptor layout + pool (via TMP) ---
	uint32_t matCount = static_cast<uint32_t>(data->materials_count) + 1;
	auto descLayout = MaterialLayout::CreateLayout(ctx.Device());
	auto descPool = MaterialLayout::CreatePool(ctx.Device(), matCount);

	auto AllocateMaterial = [&](VkImageView view, VkDescriptorSet& set) -> void {
		set = MaterialLayout::Allocate(ctx.Device(), descPool.Get(), descLayout.Get());
		MaterialLayout::Write(ctx.Device(), set, ZHLN::Vk::ImageWrite{view},
							  ZHLN::Vk::SamplerWrite{defaultSampler.Get()},
							  ZHLN::Vk::ImageWrite{shadowView.Get()},
							  ZHLN::Vk::SamplerWrite{shadowSampler.Get()});
	};

	MaterialAsset fallbackMat;
	AllocateMaterial(dummyTex.view.Get(), fallbackMat.descriptorSet);

	std::vector<MaterialAsset> materials(data->materials_count);
	for (cgltf_size i = 0; i < data->materials_count; ++i) {
		cgltf_material* mat = &data->materials[i];
		VkImageView view = dummyTex.view.Get();
		if (mat->has_pbr_metallic_roughness &&
			mat->pbr_metallic_roughness.base_color_texture.texture) {
			cgltf_texture* tex = mat->pbr_metallic_roughness.base_color_texture.texture;
			if (tex->image) {
				int idx = static_cast<int>(tex->image - data->images);
				if (textures[idx].view) {
					view = textures[idx].view.Get();
				}
			}
		}
		AllocateMaterial(view, materials[i].descriptorSet);
	}

	cgltf_free(data);

	// --- Pipelines (Via Factory & Builder) ---
	auto shaders = ZHLN::Vk::ShaderStages::FromFiles(ctx.Device(), paths.vert_spv, paths.frag_spv,
													 "VSMain", "PSMain");
	auto shadowShaders = ZHLN::Vk::ShaderStages::FromFiles(
		ctx.Device(), paths.shadow_vert_spv, paths.shadow_frag_spv, "VSMain", "PSMain");

	VkPushConstantRange mainPush = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SponzaPushConstants)};
	VkPushConstantRange shadowPush = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)};

	// Create a local variable to hold the raw handle
	VkDescriptorSetLayout rawLayout = descLayout.Get();

	// Pass the address of the local variable
	ZHLN_PipelineLayoutDesc mainLayoutDesc = {&rawLayout, 1, &mainPush, 1};
	ZHLN_PipelineLayoutDesc shadowLayoutDesc = {nullptr, 0, &shadowPush, 1};

	ZHLN::Vk::PipelineLayout pipelineLayout(
		ctx.Device(), ZHLN_CreatePipelineLayout(ctx.Device(), &mainLayoutDesc));
	ZHLN::Vk::PipelineLayout shadowLayout(
		ctx.Device(), ZHLN_CreatePipelineLayout(ctx.Device(), &shadowLayoutDesc));

	auto pipeline = ZHLN::Vk::PipelineBuilder{}
						.Shaders(shaders)
						.Layout(pipelineLayout.Get())
						.Vertex<ZHLN::Vk::Vertex>()
						.ColorFormat(VK_FORMAT_B8G8R8A8_SRGB)
						.DepthFormat(VK_FORMAT_D32_SFLOAT)
						.CullBack()
						.Build(ctx.Device());

	auto shadowPipeline = ZHLN::Vk::PipelineBuilder{}
							  .Shaders(shadowShaders)
							  .Layout(shadowLayout.Get())
							  .Vertex<ZHLN::Vk::Vertex>()
							  .DepthOnly()
							  .CullBack()
							  .Build(ctx.Device());

	PipelineSet activePipelines = {.pipeline = pipeline.Get(),
								   .shadowPipeline = shadowPipeline.Get(),
								   .pipelineLayout = pipelineLayout.Get(),
								   .shadowLayout = shadowLayout.Get()};

	SceneBuffers sceneBuffers = {.vbo = vbo.Handle(),
								 .ibo = ibo.Handle(),
								 .drawCalls = scene.drawCalls,
								 .materials = materials,
								 .fallbackMat = fallbackMat};

	// --- Frame loop resources ---
	FrameResources frame;
	auto sync = ZHLN::Vk::FrameSync<3>::Create(ctx.Device());
	auto pools =
		ZHLN::Vk::CommandPools<3>::Create(ctx.Device(), ctx.PhysicalInfo().graphics_family);

	uint32_t frame_index = 0;
	win.resized = true;
	FpsCounter fpsCounter;
	auto startTime = std::chrono::high_resolution_clock::now();

	std::println("Rendering started.");

	while (win.running) {
		ZHLN::Demo::ProcessEvents(win);
		if (win.width == 0 || win.height == 0)
			continue;
		if (win.resized)
			frame.Rebuild(ctx, allocator, win, surface.Get());

		float time =
			std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - startTime)
				.count();

		// Camera
		float camX = std::sin(time * 0.1f) * 12.0f;
		float camY = 2.5f;
		float camZ = 0.0f;
		float lookX = camX + std::cos(time * 0.1f) * 2.0f;

		Mat4 view = LookAt({camX, camY, camZ}, {lookX, camY, 0.0f}, {0.0f, 1.0f, 0.0f});
		Mat4 proj = Perspective(1.0472f, (float)win.width / (float)win.height, 0.1f, 1000.0f);
		Mat4 viewProj = Multiply(proj, view);

		Mat4 lightView = LookAt({15.f, 30.f, 9.f}, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
		Mat4 lightProj = Ortho(-25.f, 25.f, -25.f, 25.f, -50.f, 100.f);
		Mat4 lightSpace = Multiply(lightProj, lightView);

		// FPS
		if (auto title = fpsCounter.Tick(scene.drawCalls.size()); !title.empty())
			ZHLN::Demo::UpdateWindowTitle(win, title.c_str());

		// Frame sync
		const ZHLN_FrameSync& frame_sync = sync[frame_index];
		VkCommandBuffer cmd = pools.Cmd(frame_index);
		ZHLN_WaitAndResetFrame(ctx.Device(), frame_sync.in_flight, &pools[frame_index]);

		uint32_t image_index = 0;
		ZHLN_AcquireDesc acq = {.swapchain = frame.swapchain.Get().handle,
								.image_available = frame_sync.image_available,
								.timeout_ns = UINT64_MAX};
		if (ZHLN_AcquireImage(ctx.Device(), &acq, &image_index) == ZHLN_FrameResult_OutOfDate) {
			win.resized = true;
			continue;
		}

		ZHLN_BeginCommandBuffer(cmd);

		RecordFrame({
			.cmd = cmd,
			.swapchainImage = frame.swapchain.Get().images[image_index],
			.swapchainView = frame.swapchain.Get().views[image_index],
			.extent = frame.swapchain.Get().extent,
			.depthImage = frame.depthImage.Handle(),
			.depthView = frame.depthView.Get(),
			.depthInitialized = frame.depthInitialized,
			.shadowImage = shadowImage.Handle(),
			.shadowView = shadowView.Get(),
			.pipelines = activePipelines,
			.scene = sceneBuffers,
			.viewProj = viewProj,
			.lightSpaceMatrix = lightSpace,
			.camPos = {camX, camY, camZ},
		});

		ZHLN_EndCommandBuffer(cmd);

		VkCommandBufferSubmitInfo cmdInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr,
											 cmd};
		VkSemaphoreSubmitInfo waitInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr,
										  frame_sync.image_available, 0,
										  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
		VkSemaphoreSubmitInfo signalInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr,
											frame.presentSemaphores[image_index], 0,
											VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT};
		VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
								.waitSemaphoreInfoCount = 1,
								.pWaitSemaphoreInfos = &waitInfo,
								.commandBufferInfoCount = 1,
								.pCommandBufferInfos = &cmdInfo,
								.signalSemaphoreInfoCount = 1,
								.pSignalSemaphoreInfos = &signalInfo};
		vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, frame_sync.in_flight);

		ZHLN_PresentDesc pres = {.present_queue = ctx.PresentQueue(),
								 .swapchain = frame.swapchain.Get().handle,
								 .render_finished = frame.presentSemaphores[image_index],
								 .image_index = image_index};
		if (ZHLN_PresentFrame(&pres) != ZHLN_FrameResult_Ok)
			win.resized = true;

		frame_index = (frame_index + 1) % 3;
	}

	vkDeviceWaitIdle(ctx.Device());

	// All Vulkan resources automatically destroyed via RAII wrappers
	ZHLN::Demo::DestroyWindow(win);
	return 0;
}