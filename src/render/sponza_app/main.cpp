#include "../cube_app/math.hpp"
#include "Allocator.hpp"
#include "DescriptorLayout.hpp"
#include "PipelineBuilder.hpp"
#include "RenderCore.hpp"
#include "SamplerBuilder.hpp"
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
// Types & RAII Wrappers
// ============================================================================

struct GltfData {
	cgltf_data* ptr = nullptr;
	~GltfData() {
		if (ptr)
			cgltf_free(ptr);
	}

	[[nodiscard]] static GltfData Load(const std::string& path) {
		GltfData d;
		cgltf_options opts{};
		if (cgltf_parse_file(&opts, path.c_str(), &d.ptr) != cgltf_result_success ||
			cgltf_load_buffers(&opts, d.ptr, path.c_str()) != cgltf_result_success) {
			if (d.ptr) {
				cgltf_free(d.ptr);
				d.ptr = nullptr;
			}
		}
		return d;
	}

	[[nodiscard]] bool Valid() const { return ptr != nullptr; }
	cgltf_data* operator->() const { return ptr; }
};

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

// With bindless, a material no longer holds a descriptor set.
// It just holds the integer index into the global texture array.
struct MaterialAsset {
	uint32_t albedoIdx;
	uint32_t normalIdx;
	uint32_t pbrIdx;
};

struct SponzaPushConstants {
	Mat4 mvp;
	Mat4 lightSpaceMatrix;
	float camPos[4]; // 16-byte aligned
	float lightDir[4];
	uint32_t albedoIdx; // 4 bytes
	uint32_t normalIdx; // 4 bytes
	uint32_t pbrIdx;
	float _padding; // Pad to 16 bytes
};

// Bindless Scene Layout
using SceneLayout =
	ZHLN::Vk::DescriptorLayout<ZHLN::Vk::BindlessSampledImageSlot<0, 4096>, // Global Textures Array
							   ZHLN::Vk::SamplerSlot<1>, // Global Default Sampler

							   // Note: Shadow mapping is intentionally kept outside the bindless
							   // array because it uses a SamplerComparisonState (depth compare) and
							   // is a unique render target rather than a generic material property.
							   ZHLN::Vk::SampledImageSlot<2>, // Shadow Map
							   ZHLN::Vk::SamplerSlot<3>		  // Shadow Sampler
							   >;

// ============================================================================
// Helpers & Path Resolution
// ============================================================================

static std::vector<uint32_t> LoadSpirv(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::ate | std::ios::binary);
	if (!file.is_open())
		return {};
	size_t size = static_cast<size_t>(file.tellg());
	std::vector<uint32_t> buffer(size / sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(buffer.data()), size);
	return buffer;
}

[[nodiscard]] static bool FileExists(const std::string& path) {
	bool exists = std::filesystem::exists(path);
	if (!exists)
		std::println(stderr, "ERROR: File not found: {}", std::filesystem::absolute(path).string());
	return exists;
}

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
#ifdef SHADER_DIR
	p.shader_prefix = SHADER_DIR;
#else
	p.shader_prefix = "./";
#endif

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

// Memory-based Uploader
static TextureAsset UploadTexture(ZHLN::Vk::Allocator& allocator, const ZHLN::Vk::Context& ctx,
								  const VkImageCreateInfo& baseInfo, const void* pixelData,
								  uint32_t tw, uint32_t th) {
	TextureAsset result;

	// 1. Calculate Mip Levels
	uint32_t mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(tw, th)))) + 1;

	ZHLN::Vk::CommandPool batchPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	if (!batchPool.Allocate(1))
		return result;
	VkCommandBuffer cmd = batchPool[0];
	ZHLN_BeginCommandBuffer(cmd);

	VkImageCreateInfo texInfo = baseInfo;
	texInfo.extent = {tw, th, 1};
	texInfo.mipLevels = mipLevels; // Set the calculated levels
	// Add SRC bit so we can read from levels to blit them
	texInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	result.image = ZHLN::Vk::Image::Create(allocator.Get(), texInfo, VMA_MEMORY_USAGE_GPU_ONLY);

	size_t imageSize = tw * th * 4;
	auto staging = ZHLN::Vk::Buffer::Create(
		allocator.Get(), imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	memcpy(staging.Map().data, pixelData, imageSize);

	// Transition ALL levels to DST_OPTIMAL initially
	ZHLN_ImageBarrierDesc initialBarrier = {.image = result.image.Handle(),
											.src_access = 0,
											.dst_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
											.src_layout = VK_IMAGE_LAYOUT_UNDEFINED,
											.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
											.src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
											.dst_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
											.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
											.base_mip = 0,
											.mip_count = mipLevels};
	ZHLN_CmdImageBarrier(cmd, &initialBarrier);

	ZHLN::Vk::CopyBufferToImage(cmd, {.buffer = staging.Handle(),
									  .image = result.image.Handle(),
									  .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
									  .width = tw,
									  .height = th});

	// Generate the Mips! (This handles transitions to SHADER_READ_ONLY)
	ZHLN::Vk::GenerateMipmaps(cmd, result.image.Handle(), tw, th);

	ZHLN_EndCommandBuffer(cmd);

	VkCommandBufferSubmitInfo subInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
										 .commandBuffer = cmd};
	VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
							.commandBufferInfoCount = 1,
							.pCommandBufferInfos = &subInfo};
	vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(ctx.GraphicsQueue());

	result.view = ZHLN::Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(
		ctx.Device(), result.image.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);
	return result;
}

// File-based wrapper
static TextureAsset UploadTexture(ZHLN::Vk::Allocator& allocator, ZHLN::Demo::WindowState& win,
								  const ZHLN::Vk::Context& ctx, const VkImageCreateInfo& baseInfo,
								  const std::string& fullPath) {
	int tw, th, tc;
	stbi_uc* pixels = stbi_load(fullPath.c_str(), &tw, &th, &tc, STBI_rgb_alpha);
	if (!pixels)
		return {};

	TextureAsset result = UploadTexture(allocator, ctx, baseInfo, pixels, static_cast<uint32_t>(tw),
										static_cast<uint32_t>(th));
	stbi_image_free(pixels);
	return result;
}

// ============================================================================
// FPS counter
// ============================================================================

struct FpsCounter {
	std::chrono::high_resolution_clock::time_point lastUpdate =
		std::chrono::high_resolution_clock::now();
	uint32_t frames = 0;

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
								   .old_swapchain = swapchain.Get().handle};
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
	VkDescriptorSet globalSet; // Passed from main
};

struct FrameRecordDesc {
	VkCommandBuffer cmd;
	VkImage swapchainImage;
	VkImageView swapchainView;
	VkExtent2D extent;

	VkImage depthImage;
	VkImageView depthView;

	const VkImage shadowImage;
	VkImageView shadowView;
	VkExtent2D shadowExtent;

	const PipelineSet& pipelines;
	const SceneBuffers& scene;

	Mat4 viewProj;
	Mat4 lightSpaceMatrix;
	float camPos[3];
};

static void RecordShadowPass(VkCommandBuffer cmd, const void* pUserData) {
	const auto& d = *static_cast<const FrameRecordDesc*>(pUserData);

	ZHLN_RenderPassDesc shadowPass = {.target_view = VK_NULL_HANDLE,
									  .depth_view = d.shadowView,
									  .extent = d.shadowExtent,
									  .clear_depth = 1.0f};
	ZHLN::Vk::ScopedRendering render(cmd, shadowPass);

	VkBuffer vboHandle = d.scene.vbo;
	VkDeviceSize offset = 0;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipelines.shadowPipeline);
	vkCmdBindVertexBuffers(cmd, 0, 1, &vboHandle, &offset);
	vkCmdBindIndexBuffer(cmd, d.scene.ibo, 0, VK_INDEX_TYPE_UINT32);

	for (const auto& draw : d.scene.drawCalls) {
		Mat4 shadowMVP = Multiply(d.lightSpaceMatrix, draw.worldMatrix);
		ZHLN::Vk::Push(cmd, d.pipelines.shadowLayout, VK_SHADER_STAGE_VERTEX_BIT, shadowMVP);
		vkCmdDrawIndexed(cmd, draw.mesh->indexCount, 1, draw.mesh->firstIndex, 0, 0);
	}
}

static void RecordMainPass(VkCommandBuffer cmd, const void* pUserData) {
	const auto& d = *static_cast<const FrameRecordDesc*>(pUserData);

	ZHLN_RenderPassDesc mainPass = {.target_view = d.swapchainView,
									.depth_view = d.depthView,
									.extent = d.extent,
									.clear_color = {0.5f, 0.7f, 1.0f, 1.0f},
									.clear_depth = 1.0f};
	ZHLN::Vk::ScopedRendering render(cmd, mainPass);

	VkBuffer vboHandle = d.scene.vbo;
	VkDeviceSize offset = 0;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipelines.pipeline);
	vkCmdBindVertexBuffers(cmd, 0, 1, &vboHandle, &offset);
	vkCmdBindIndexBuffer(cmd, d.scene.ibo, 0, VK_INDEX_TYPE_UINT32);

	// Bindless: Global set bound once
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipelines.pipelineLayout, 0, 1,
							&d.scene.globalSet, 0, nullptr);

	for (const auto& draw : d.scene.drawCalls) {
		// Fetch the material (or fallback if none assigned)
		const auto& mat = (draw.mesh->materialIndex >= 0)
							  ? d.scene.materials[draw.mesh->materialIndex]
							  : d.scene.fallbackMat;

		SponzaPushConstants pc = {
			.mvp = Multiply(d.viewProj, draw.worldMatrix),
			.lightSpaceMatrix = Multiply(d.lightSpaceMatrix, draw.worldMatrix),
			.camPos = {d.camPos[0], d.camPos[1], d.camPos[2], 1.0f}, // Set W to 1.0
			.lightDir = {-0.5f, -1.0f, -0.3f, 0.0f}, // Pointing down and slightly to the side
			.albedoIdx = mat.albedoIdx,
			.normalIdx = mat.normalIdx,
		};

		ZHLN::Vk::Push(cmd, d.pipelines.pipelineLayout,
					   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, pc);

		vkCmdDrawIndexed(cmd, draw.mesh->indexCount, 1, draw.mesh->firstIndex, 0, 0);
	}
}

static void RecordFrame(const FrameRecordDesc& d) {
	using namespace ZHLN::Vk;

	// --- 1. Define all transitions on the stack ---

	const PassResource shadowBarriers[] = {
		{.barrier = {.image = d.shadowImage,
					 .src_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
					 .dst_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
					 .src_layout = VK_IMAGE_LAYOUT_UNDEFINED,
					 .dst_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
					 .src_stage = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
					 .dst_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
					 .aspect = VK_IMAGE_ASPECT_DEPTH_BIT}}};

	const PassResource mainBarriers[] = {
		{.barrier = {.image = d.swapchainImage,
					 .src_access = VK_ACCESS_2_NONE,
					 .dst_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
					 .src_layout = VK_IMAGE_LAYOUT_UNDEFINED,
					 .dst_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					 .src_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
					 .dst_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
					 .aspect = VK_IMAGE_ASPECT_COLOR_BIT}},
		{.barrier = {.image = d.depthImage,
					 .src_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
					 .dst_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
					 .src_layout = VK_IMAGE_LAYOUT_UNDEFINED,
					 .dst_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
					 .src_stage = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
					 .dst_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
					 .aspect = VK_IMAGE_ASPECT_DEPTH_BIT}},
		{.barrier = {.image = d.shadowImage,
					 .src_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
					 .dst_access = VK_ACCESS_2_SHADER_READ_BIT, // Keep this
					 .src_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
					 .dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					 .src_stage =
						 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, // After shadow pass finishes
					 // FIX: Change this stage to FRAGMENT_SHADER.
					 // This is the stage where we actually perform the read.
					 .dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
					 .aspect = VK_IMAGE_ASPECT_DEPTH_BIT}},
	};

	const PassResource presentBarriers[] = {
		{.barrier = {.image = d.swapchainImage,
					 .src_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
					 .dst_access = VK_ACCESS_2_NONE,
					 .src_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					 .dst_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
					 .src_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
					 .dst_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
					 .aspect = VK_IMAGE_ASPECT_COLOR_BIT}}};

	// --- 2. Define the passes on the stack ---

	const PassDesc passes[] = {{.name = "Shadow Map Pass",
								.transitions = shadowBarriers,
								.record = RecordShadowPass,
								.pUserData = &d},
							   {.name = "Main Scene Pass",
								.transitions = mainBarriers,
								.record = RecordMainPass,
								.pUserData = &d},
							   {.name = "Present Handoff Pass",
								.transitions = presentBarriers,
								.record = nullptr, // Transition only
								.pUserData = nullptr}};

	// --- 3. Execute ---
	ZHLN::Vk::ExecutePasses(d.cmd, passes);
}

// ============================================================================
// main
// ============================================================================

auto main() -> int {
	const AssetPaths paths = ResolvePaths();
	ZHLN::Demo::WindowState win =
		ZHLN::Demo::InitWindow(1280, 720, "ZHLN Engine - Sponza Bindless");

	if (!win.os_window) {
		std::println(stderr, "Failed to create OS window. Exiting.");
		return -1;
	}

	// --- 1. Instance Setup ---
	ZHLN_InstanceDesc inst_desc = ZHLN_VERBOSE_INSTANCE_DESC;
	std::vector<const char*> inst_exts = ZHLN::Demo::GetRequiredInstanceExtensions();
	inst_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	inst_exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
	inst_exts.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
#ifdef __APPLE__
	inst_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

	inst_desc.extensions = inst_exts.data();
	inst_desc.extension_count = static_cast<uint32_t>(inst_exts.size());

	// --- 2. Feature & Extension Negotiation ---
	bool has_maint1 =
		ZHLN::Vk::IsInstanceExtensionSupported(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);

	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swap_maint = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
		.swapchainMaintenance1 = VK_TRUE};

	VkPhysicalDeviceVulkan13Features feat13 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.pNext = has_maint1 ? &swap_maint : nullptr,
		.shaderDemoteToHelperInvocation = VK_TRUE,
		.synchronization2 = VK_TRUE,
		.dynamicRendering = VK_TRUE,
	};

	// Bindless Feature Enablement
	VkPhysicalDeviceVulkan12Features feat12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &feat13,
		.descriptorIndexing = VK_TRUE,
		.shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
		.descriptorBindingPartiallyBound = VK_TRUE,
		.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
		.runtimeDescriptorArray = VK_TRUE,
		.bufferDeviceAddress = VK_TRUE};

	VkPhysicalDeviceFeatures2 feat2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
									   .pNext = &feat12,
									   .features = {.samplerAnisotropy = VK_TRUE}};

	// --- 3. Device Setup ---
	std::vector<const char*> dev_exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	if (has_maint1) {
		dev_exts.push_back(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
		dev_exts.push_back(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME);
	}
#ifdef __APPLE__
	dev_exts.push_back("VK_KHR_portability_subset");
#endif

	ZHLN_DeviceDesc dev_desc = {.extensions = dev_exts.data(),
								.extension_count = static_cast<uint32_t>(dev_exts.size()),
								.features = &feat2,
								.enable_validation = true};

	// --- 4. Context Creation ---
	auto ctx = ZHLN::Vk::Context::Create(inst_desc, {VK_NULL_HANDLE, VK_NULL_HANDLE}, dev_desc);

	if (!ctx) {
		std::println(stderr, "Context creation failed. Check if your GPU supports Vulkan 1.3 and "
							 "Descriptor Indexing.");
		return -1;
	}

	VkSurfaceKHR raw_surface = ZHLN::Demo::CreateSurface(ctx.Instance(), win);
	ZHLN::Vk::Surface surface(ctx.Instance(), raw_surface);

	ZHLN::Vk::Allocator allocator;
	if (!allocator.Init(ctx)) {
		std::println("Allocator creation failed");
		return -1;
	}
	if (!paths.Validate()) {
		std::println("Path validation unsatisfied");
		return -1;
	}

	// --- Load glTF ---
	std::println("Loading Sponza...");
	auto gltf = GltfData::Load(paths.gltf);
	if (!gltf.Valid()) {
		std::println(stderr, "FATAL: Failed to load glTF.");
		return -1;
	}
	Scene scene = BuildScene(gltf.ptr);

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

	ZHLN_EndCommandBuffer(setupCmd);
	VkCommandBufferSubmitInfo setupCmdInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
											  .commandBuffer = setupCmd};
	VkSubmitInfo2 setupSubmit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
								 .commandBufferInfoCount = 1,
								 .pCommandBufferInfos = &setupCmdInfo};
	vkQueueSubmit2(ctx.GraphicsQueue(), 1, &setupSubmit, VK_NULL_HANDLE);
	vkQueueWaitIdle(ctx.GraphicsQueue());

	// --- Dummy 1x1 white texture ---
	VkImageCreateInfo texBaseInfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
									 .imageType = VK_IMAGE_TYPE_2D,
									 .format = VK_FORMAT_R8G8B8A8_UNORM,
									 .mipLevels = 1,
									 .arrayLayers = 1,
									 .samples = VK_SAMPLE_COUNT_1_BIT,
									 .tiling = VK_IMAGE_TILING_OPTIMAL,
									 .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
											  VK_IMAGE_USAGE_SAMPLED_BIT,
									 .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
									 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};

	uint32_t white_pixel = 0xFFFFFFFF;
	TextureAsset dummyTex = UploadTexture(allocator, ctx, texBaseInfo, &white_pixel, 1, 1);

	// --- Textures Loop ---
	std::vector<TextureAsset> textures(gltf->images_count);
	std::println("Uploading {} textures...", gltf->images_count);
	for (cgltf_size i = 0; i < gltf->images_count; ++i) {
		ZHLN::Demo::ProcessEvents(win);
		std::string fullPath = paths.asset_prefix + gltf->images[i].uri;
		textures[i] = UploadTexture(allocator, win, ctx, texBaseInfo, fullPath);
		std::println("  [{}/{}] {}", i + 1, gltf->images_count, gltf->images[i].uri);
	}

	// --- Shadow map ---
	constexpr uint32_t SHADOW_RES = 4096;
	VkImageCreateInfo shadowInfo = texBaseInfo;
	shadowInfo.format = VK_FORMAT_D32_SFLOAT;
	shadowInfo.extent = {SHADOW_RES, SHADOW_RES, 1};
	shadowInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

	auto shadowImage =
		ZHLN::Vk::Image::Create(allocator.Get(), shadowInfo, VMA_MEMORY_USAGE_GPU_ONLY);
	auto shadowView =
		ZHLN::Vk::CreateView<VK_FORMAT_D32_SFLOAT>(ctx.Device(), shadowImage.Handle());
	VkExtent2D shadowExtent = {SHADOW_RES, SHADOW_RES};

	// --- Samplers (Via Builder) ---
	auto defaultSampler =
		ZHLN::Vk::SamplerBuilder{}.Linear().Repeat().Anisotropy(8.0f).Build(ctx.Device());
	auto shadowSampler = ZHLN::Vk::SamplerBuilder{}
							 .Linear()
							 .ClampToBorder(VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE)
							 .DepthCompare()
							 .Build(ctx.Device());

	// --- Global Descriptor Set Layout & Allocation ---
	auto descLayout = SceneLayout::CreateLayout(ctx.Device());
	auto descPool = SceneLayout::CreatePool(ctx.Device(), 1);

	VkDescriptorSet globalSet =
		SceneLayout::Allocate(ctx.Device(), descPool.Get(), descLayout.Get());

	SceneLayout::Write(ctx.Device(), globalSet, ZHLN::Vk::SkipWrite{},
					   ZHLN::Vk::SamplerWrite{defaultSampler.Get()},
					   ZHLN::Vk::ImageWrite{shadowView.Get()},
					   ZHLN::Vk::SamplerWrite{shadowSampler.Get()});

	// 1. Define the Registry tied to the Layout and specific Binding Index (0)
	ZHLN::Vk::BindlessRegistry<SceneLayout, 0> bindless;

	// 2. Initialize (Binding index and capacity are deduced from the template)
	bindless.Init(ctx.Device(), globalSet);

	// 3. Registering remains runtime-dynamic but type-safe
	uint32_t fallbackTexIdx = bindless.RegisterImage(dummyTex.view.Get());

	std::println("Bindless Registry Initialized. Capacity: {}", bindless.Capacity());

	// Register loaded gltf images
	std::vector<uint32_t> bindlessTextureIndices(gltf->images_count, fallbackTexIdx);
	for (cgltf_size i = 0; i < gltf->images_count; ++i) {
		if (textures[i].view) {
			bindlessTextureIndices[i] = bindless.RegisterImage(textures[i].view.Get());
		}
	}

	// Setup materials to point to correct bindless index
	std::vector<MaterialAsset> materials(gltf->materials_count);
	for (cgltf_size i = 0; i < gltf->materials_count; ++i) {
		cgltf_material* mat = &gltf->materials[i];

		// Default both to fallback (white 1x1)
		materials[i].albedoIdx = fallbackTexIdx;
		materials[i].normalIdx = fallbackTexIdx;
		materials[i].pbrIdx = fallbackTexIdx;

		// 1. Assign Albedo (Base Color)
		if (mat->has_pbr_metallic_roughness &&
			mat->pbr_metallic_roughness.base_color_texture.texture) {
			cgltf_texture* tex = mat->pbr_metallic_roughness.base_color_texture.texture;
			if (tex->image) {
				int imgIdx = static_cast<int>(tex->image - gltf->images);
				materials[i].albedoIdx = bindlessTextureIndices[imgIdx];
			}
		}

		if (mat->has_pbr_metallic_roughness &&
			mat->pbr_metallic_roughness.metallic_roughness_texture.texture) {
			cgltf_texture* tex = mat->pbr_metallic_roughness.metallic_roughness_texture.texture;
			if (tex->image) {
				int imgIdx = static_cast<int>(tex->image - gltf->images);
				materials[i].pbrIdx = bindlessTextureIndices[imgIdx];
			}
		}

		// 2. Assign Normal Map
		if (mat->normal_texture.texture) {
			cgltf_texture* tex = mat->normal_texture.texture;
			if (tex->image) {
				int imgIdx = static_cast<int>(tex->image - gltf->images);
				materials[i].normalIdx = bindlessTextureIndices[imgIdx];
			}
		}
	}
	MaterialAsset fallbackMat = {.albedoIdx = fallbackTexIdx, .normalIdx = fallbackTexIdx};

	// --- Pipelines (Via Factory & Builder) ---
	auto shaders = ZHLN::Vk::ShaderStages::FromFiles(ctx.Device(), paths.vert_spv, paths.frag_spv,
													 "VSMain", "PSMain");
	auto shadowShaders = ZHLN::Vk::ShaderStages::FromFiles(
		ctx.Device(), paths.shadow_vert_spv, paths.shadow_frag_spv, "VSMain", "PSMain");

	// Note: mainPush now needs FRAGMENT bit to read textureIndex
	VkPushConstantRange mainPush = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
									sizeof(SponzaPushConstants)};
	VkPushConstantRange shadowPush = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)};

	VkDescriptorSetLayout rawLayout = descLayout.Get();
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
							  .CullNone()
							  .Build(ctx.Device());

	PipelineSet activePipelines = {.pipeline = pipeline.Get(),
								   .shadowPipeline = shadowPipeline.Get(),
								   .pipelineLayout = pipelineLayout.Get(),
								   .shadowLayout = shadowLayout.Get()};

	SceneBuffers sceneBuffers = {.vbo = vbo.Handle(),
								 .ibo = ibo.Handle(),
								 .drawCalls = scene.drawCalls,
								 .materials = materials,
								 .fallbackMat = fallbackMat,
								 .globalSet = globalSet};

	// --- Frame loop resources ---
	FrameResources frame;
	auto sync = ZHLN::Vk::FrameSync<3>::Create(ctx.Device());
	auto pools =
		ZHLN::Vk::CommandPools<3>::Create(ctx.Device(), ctx.PhysicalInfo().graphics_family);

	uint32_t frame_index = 0;
	win.resized = true;
	FpsCounter fpsCounter;
	auto startTime = std::chrono::high_resolution_clock::now();

	std::println("Rendering started (Bindless Architecture).");

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

		// 1. Move the sun further away so it sees the whole scene
		// Eye = (40, 60, 40), Center = (0, 10, 0)
		Mat4 lightView = LookAt({40.f, 60.f, 40.f}, {0.f, 10.f, 0.f}, {0.f, 1.f, 0.f});

		// 2. Huge Ortho box (160 units wide, 300 units deep)
		Mat4 lightProj = Ortho(-80.f, 80.f, -80.f, 80.f, 0.1f, 300.f);

		// 3. Matrix order (Proj * View)
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
			.shadowImage = shadowImage.Handle(),
			.shadowView = shadowView.Get(),
			.shadowExtent = shadowExtent,
			.pipelines = activePipelines,
			.scene = sceneBuffers,
			.viewProj = viewProj,
			.lightSpaceMatrix = lightSpace,
			.camPos = {camX, camY, camZ},
		});

		ZHLN_EndCommandBuffer(cmd);

		ZHLN_FrameSubmitDesc submitDesc = {.graphicsQueue = ctx.GraphicsQueue(),
										   .presentQueue = ctx.PresentQueue(),
										   .cmd = cmd,
										   .imageAvailable = frame_sync.image_available,
										   .renderFinished = frame.presentSemaphores[image_index],
										   .inFlight = frame_sync.in_flight,
										   .swapchain = frame.swapchain.Get().handle,
										   .imageIndex = image_index};

		if (ZHLN::Vk::SubmitAndPresent(submitDesc) != ZHLN_FrameResult_Ok) {
			win.resized = true;
		}

		frame_index = (frame_index + 1) % 3;
	}

	vkDeviceWaitIdle(ctx.Device());

	ZHLN::Demo::DestroyWindow(win);
	return 0;
}