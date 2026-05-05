#include "../cube_app/math.hpp"
#include "Allocator.hpp"
#include "RenderCore.hpp"
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
#include <fstream>
#include <print>
#include <vector>

// ----------------------------------------------------------------------------
// Vertex Definition & Reflection
// ----------------------------------------------------------------------------
struct Vertex {
	std::array<float, 3> pos;
	std::array<float, 3> norm;
	std::array<float, 2> uv;
};
ZHLN_REFLECT_VERTEX(Vertex, pos, norm, uv);

// ----------------------------------------------------------------------------
// Types
// ----------------------------------------------------------------------------

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
	VkImageView view = VK_NULL_HANDLE;
};

struct MaterialAsset {
	VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
};

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------
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

static VkImageView CreateImageView(VkDevice device, VkImage image, VkFormat format,
								   VkImageAspectFlags aspect) {
	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.subresourceRange = {.aspectMask = aspect, .levelCount = 1, .layerCount = 1},
	};
	VkImageView view;
	vkCreateImageView(device, &view_info, nullptr, &view);
	return view;
}

// Helper to check if a file exists before trying to load it
[[nodiscard]]
bool FileExists(const std::string& path) {
	bool exists = std::filesystem::exists(path);
	if (!exists) {
		std::println(stderr, "ERROR: File not found: {}", std::filesystem::absolute(path).string());
	}
	return exists;
}

// ----------------------------------------------------------------------------
// Main Application
// ----------------------------------------------------------------------------
auto main() -> int {
	// Path Configuration
	// If you run from the root, these paths work.
	// If you run from build/src/render, you need "../../.."
	std::string asset_prefix = "resources/assets/main_sponza/";
	std::string shader_prefix = ""; // Shaders are usually in the same dir as the exe
	// Smart Logic: Check if we are running from root or from the build folder
	if (std::filesystem::exists("build/src/render/sponza.hlsl.VSMain.spv")) {
		// We are in the ROOT
		shader_prefix = "build/src/render/";
	} else if (std::filesystem::exists("../../../resources")) {
		// We are in build/src/render
		asset_prefix = "../../../resources/assets/main_sponza/";
		shader_prefix = "./";
	} else {
		// Fallback: assume local (current folder)
		shader_prefix = "./";
	}
	ZHLN::Demo::WindowState win = ZHLN::Demo::InitWindow(1280, 720, "ZHLN Engine - Sponza Atrium");

	// --- 1. Init Vulkan Context ---
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
	if (!allocator.Init(ctx)) {
		return -1;
	}

	// --- CHECK ASSETS ---
	std::string gltf_path = asset_prefix + "NewSponza_Main_glTF_003.gltf";
	std::string v_shader = shader_prefix + "sponza.hlsl.VSMain.spv";
	std::string f_shader = shader_prefix + "sponza.hlsl.PSMain.spv";

	if (!FileExists(gltf_path)) {
		std::println(stderr, "FATAL: Could not find GLTF at {}", gltf_path);
		return -1;
	}
	if (!FileExists(v_shader)) {
		std::println(stderr, "FATAL: Could not find Shaders at {}", v_shader);
		return -1;
	}

	if (!FileExists(f_shader)) {
		std::println(stderr, "FATAL: Could not find Shaders at {}", v_shader);
		return -1;
	}

	// --- 2. Load glTF Data ---
	std::println("Loading Sponza... This might take a few seconds.");

	cgltf_options options = {};
	cgltf_data* data = nullptr;
	if (cgltf_parse_file(&options, gltf_path.c_str(), &data) != cgltf_result_success ||
		cgltf_load_buffers(&options, data, gltf_path.c_str()) != cgltf_result_success) {
		std::println(stderr, "FATAL: Failed to load glTF file.");
		return -1;
	}

	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<MeshPrimitive> gpu_primitives;
	std::vector<DrawCall> scene_draw_calls;

	std::vector<std::vector<MeshPrimitive>> mesh_to_primitives(data->meshes_count);

	// Extract Geometry
	for (cgltf_size i = 0; i < data->meshes_count; ++i) {
		for (cgltf_size j = 0; j < data->meshes[i].primitives_count; ++j) {
			cgltf_primitive* prim = &data->meshes[i].primitives[j];

			uint32_t firstIndex = static_cast<uint32_t>(indices.size());
			uint32_t vertexOffset = static_cast<uint32_t>(vertices.size());
			uint32_t indexCount = static_cast<uint32_t>(prim->indices->count);

			// Indices
			for (cgltf_size k = 0; k < prim->indices->count; ++k) {
				indices.push_back(cgltf_accessor_read_index(prim->indices, k) + vertexOffset);
			}

			// Vertices
			size_t vertexCount = prim->attributes[0].data->count;
			size_t startVert = vertices.size();
			vertices.resize(vertices.size() + vertexCount);

			for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
				cgltf_attribute* attr = &prim->attributes[a];
				for (cgltf_size v = 0; v < vertexCount; ++v) {
					if (attr->type == cgltf_attribute_type_position) {
						cgltf_accessor_read_float(attr->data, v, vertices[startVert + v].pos.data(),
												  3);
					} else if (attr->type == cgltf_attribute_type_normal) {
						cgltf_accessor_read_float(attr->data, v,
												  vertices[startVert + v].norm.data(), 3);
					} else if (attr->type == cgltf_attribute_type_texcoord) {
						cgltf_accessor_read_float(attr->data, v, vertices[startVert + v].uv.data(),
												  2);
					}
				}
			}

			// Material Index mapping
			int matIdx = -1;
			if (prim->material) {
				matIdx = static_cast<int>(prim->material - data->materials);
			}
			MeshPrimitive p = {.indexCount = (uint32_t)prim->indices->count,
							   .firstIndex = firstIndex,
							   .materialIndex =
								   prim->material ? (int)(prim->material - data->materials) : -1};
			mesh_to_primitives[i].push_back(p);
		}
	}

	// We walk every node in the glTF. If a node has a mesh, we create a DrawCall.
	for (cgltf_size i = 0; i < data->nodes_count; ++i) {
		cgltf_node* node = &data->nodes[i];
		if (!node->mesh)
			continue;

		// Get the world matrix for this node
		float matrix[16];
		cgltf_node_transform_world(node, matrix);

		// Convert cgltf float[16] to our Mat4
		Mat4 worldMat;
		std::copy(matrix, matrix + 16, worldMat.data.begin());

		// Map this node to its primitives
		size_t meshIdx = node->mesh - data->meshes;
		for (auto& prim : mesh_to_primitives[meshIdx]) {
			scene_draw_calls.push_back({&prim, worldMat});
		}
	}

	// --- 3. Upload Buffers & Textures ---
	ZHLN::Vk::CommandPool setupPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	if (!setupPool.Allocate(1))
		return -1;
	VkCommandBuffer setupCmd = setupPool[0];
	ZHLN_BeginCommandBuffer(setupCmd);

	// VBO / IBO
	auto vbo = ZHLN::Vk::Buffer::Create(allocator.Get(), vertices.size() * sizeof(Vertex),
										VK_BUFFER_USAGE_TRANSFER_DST_BIT |
											VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
										VMA_MEMORY_USAGE_GPU_ONLY);
	auto ibo = ZHLN::Vk::Buffer::Create(allocator.Get(), indices.size() * sizeof(uint32_t),
										VK_BUFFER_USAGE_TRANSFER_DST_BIT |
											VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
										VMA_MEMORY_USAGE_GPU_ONLY);

	auto stagingVBO = ZHLN::Vk::UploadToBuffer(allocator.Get(), setupCmd, vbo, vertices.data(),
											   vertices.size() * sizeof(Vertex));
	auto stagingIBO = ZHLN::Vk::UploadToBuffer(allocator.Get(), setupCmd, ibo, indices.data(),
											   indices.size() * sizeof(uint32_t));

	// Default Dummy Texture (1x1 White)
	uint32_t white_pixel = 0xFFFFFFFF;
	VkImageCreateInfo tex_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
								  .imageType = VK_IMAGE_TYPE_2D,
								  .format = VK_FORMAT_R8G8B8A8_UNORM,
								  .extent = {1, 1, 1},
								  .mipLevels = 1,
								  .arrayLayers = 1,
								  .samples = VK_SAMPLE_COUNT_1_BIT,
								  .tiling = VK_IMAGE_TILING_OPTIMAL,
								  .usage =
									  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
								  .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
								  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	TextureAsset dummyTex;
	dummyTex.image = ZHLN::Vk::Image::Create(allocator.Get(), tex_info, VMA_MEMORY_USAGE_GPU_ONLY);
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

	// Load Actual Textures
	std::vector<TextureAsset> textures(data->images_count);

	std::println("Uploading {} textures...", data->images_count);

	for (cgltf_size i = 0; i < data->images_count; ++i) {
		// 1. Process OS Events every texture to prevent Watchdog Timeout
		ZHLN::Demo::ProcessEvents(win);

		std::string uri = data->images[i].uri;
		std::string full_path = asset_prefix + uri;

		int tw, th, tc;
		stbi_uc* pixels = stbi_load(full_path.c_str(), &tw, &th, &tc, STBI_rgb_alpha);
		if (!pixels) {
			std::println(stderr, "  Warning: Failed to load {}", uri);
			continue;
		}

		// 2. Create a fresh command pool/buffer just for this batch
		// This prevents the command buffer from becoming a multi-gigabyte monster
		ZHLN::Vk::CommandPool batchPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
		if (!batchPool.Allocate(1)) {
			std::println("Failed to allocate batch pool");
			return -1;
		}
		VkCommandBuffer cmd = batchPool[0];
		ZHLN_BeginCommandBuffer(cmd);

		// 3. Create GPU Image
		VkImageCreateInfo batch_tex_info = tex_info;
		batch_tex_info.extent = {static_cast<uint32_t>(tw), static_cast<uint32_t>(th), 1};
		textures[i].image =
			ZHLN::Vk::Image::Create(allocator.Get(), batch_tex_info, VMA_MEMORY_USAGE_GPU_ONLY);

		// 4. Create Staging Buffer
		size_t imageSize = tw * th * 4;
		auto staging =
			ZHLN::Vk::Buffer::Create(allocator.Get(), imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
									 VMA_MEMORY_USAGE_CPU_ONLY);
		memcpy(staging.Map().data, pixels, imageSize);

		// 5. Record Upload
		ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
			cmd, textures[i].image.Handle());
		ZHLN::Vk::CopyBufferToImage(cmd, {.buffer = staging.Handle(),
										  .image = textures[i].image.Handle(),
										  .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
										  .width = (uint32_t)tw,
										  .height = (uint32_t)th});
		ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
								   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			cmd, textures[i].image.Handle());

		ZHLN_EndCommandBuffer(cmd);

		// 6. Submit and Wait immediately for this texture
		VkCommandBufferSubmitInfo subInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
											 .commandBuffer = cmd};
		VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
								.commandBufferInfoCount = 1,
								.pCommandBufferInfos = &subInfo};
		vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);

		// We wait here so we can safely destroy the 'staging' buffer in the next line
		vkQueueWaitIdle(ctx.GraphicsQueue());

		stbi_image_free(pixels);
		std::println("  [{}/{}] Loaded: {}", i + 1, data->images_count, uri);

		// 'staging' and 'batchPool' go out of scope and are destroyed here, freeing RAM
	}

	ZHLN_EndCommandBuffer(setupCmd);

	// Submit and wait for all uploads
	VkCommandBufferSubmitInfo setupCmdInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
											  .commandBuffer = setupCmd};
	VkSubmitInfo2 setupSubmit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
								 .commandBufferInfoCount = 1,
								 .pCommandBufferInfos = &setupCmdInfo};
	vkQueueSubmit2(ctx.GraphicsQueue(), 1, &setupSubmit, VK_NULL_HANDLE);
	vkQueueWaitIdle(ctx.GraphicsQueue());

	// Create Views & Sampler
	dummyTex.view = CreateImageView(ctx.Device(), dummyTex.image.Handle(), VK_FORMAT_R8G8B8A8_UNORM,
									VK_IMAGE_ASPECT_COLOR_BIT);
	for (auto& tex : textures) {
		if (tex.image.Valid())
			tex.view = CreateImageView(ctx.Device(), tex.image.Handle(), VK_FORMAT_R8G8B8A8_UNORM,
									   VK_IMAGE_ASPECT_COLOR_BIT);
	}

	VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.anisotropyEnable = VK_TRUE,
		.maxAnisotropy = 8.0f // Makes Sponza look way better
	};
	VkSampler defaultSampler;
	vkCreateSampler(ctx.Device(), &sampler_info, nullptr, &defaultSampler);

	// --- 4. Descriptor Sets ---
	VkDescriptorSetLayoutBinding bindings[2] = {{.binding = 0,
												 .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
												 .descriptorCount = 1,
												 .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
												{.binding = 1,
												 .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
												 .descriptorCount = 1,
												 .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
	VkDescriptorSetLayoutCreateInfo layoutInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2,
		.pBindings = bindings};
	VkDescriptorSetLayout descLayout;
	vkCreateDescriptorSetLayout(ctx.Device(), &layoutInfo, nullptr, &descLayout);

	uint32_t matCount = static_cast<uint32_t>(data->materials_count) + 1; // +1 for fallback
	VkDescriptorPoolSize poolSizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, matCount},
										 {VK_DESCRIPTOR_TYPE_SAMPLER, matCount}};
	VkDescriptorPoolCreateInfo poolInfo = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
										   .maxSets = matCount,
										   .poolSizeCount = 2,
										   .pPoolSizes = poolSizes};
	VkDescriptorPool descPool;
	vkCreateDescriptorPool(ctx.Device(), &poolInfo, nullptr, &descPool);

	std::vector<MaterialAsset> materials(data->materials_count);

	auto AllocateMaterial = [&](VkImageView view, VkDescriptorSet& set) {
		VkDescriptorSetAllocateInfo allocInfo = {.sType =
													 VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
												 .descriptorPool = descPool,
												 .descriptorSetCount = 1,
												 .pSetLayouts = &descLayout};
		vkAllocateDescriptorSets(ctx.Device(), &allocInfo, &set);

		VkDescriptorImageInfo imageInfo = {.imageView = view,
										   .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkDescriptorImageInfo samplerDescInfo = {.sampler = defaultSampler};
		VkWriteDescriptorSet writes[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
										   .dstSet = set,
										   .dstBinding = 0,
										   .descriptorCount = 1,
										   .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
										   .pImageInfo = &imageInfo},
										  {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
										   .dstSet = set,
										   .dstBinding = 1,
										   .descriptorCount = 1,
										   .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
										   .pImageInfo = &samplerDescInfo}};
		vkUpdateDescriptorSets(ctx.Device(), 2, writes, 0, nullptr);
	};

	// Fallback Material
	MaterialAsset fallbackMat;
	AllocateMaterial(dummyTex.view, fallbackMat.descriptorSet);

	for (cgltf_size i = 0; i < data->materials_count; ++i) {
		cgltf_material* mat = &data->materials[i];
		VkImageView view = dummyTex.view; // default

		if (mat->has_pbr_metallic_roughness &&
			mat->pbr_metallic_roughness.base_color_texture.texture) {
			cgltf_texture* tex = mat->pbr_metallic_roughness.base_color_texture.texture;
			if (tex->image) {
				int imgIdx = static_cast<int>(tex->image - data->images);
				if (textures[imgIdx].view != VK_NULL_HANDLE) {
					view = textures[imgIdx].view;
				}
			}
		}
		AllocateMaterial(view, materials[i].descriptorSet);
	}

	cgltf_free(data);

	// --- 5. Pipelines ---
	auto vert_code = LoadSpirv("sponza.hlsl.VSMain.spv");
	auto frag_code = LoadSpirv("sponza.hlsl.PSMain.spv");
	ZHLN_ShaderDesc v_desc = {vert_code.data(), vert_code.size() * 4, "VSMain"};
	ZHLN_ShaderDesc f_desc = {frag_code.data(), frag_code.size() * 4, "PSMain"};
	auto shaders = ZHLN::Vk::ShaderStages::Create(ctx.Device(), v_desc, f_desc);

	VkPushConstantRange push_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(Mat4)};
	ZHLN_PipelineLayoutDesc pLayoutDesc = {.set_layouts = &descLayout,
										   .set_layout_count = 1,
										   .push_constants = &push_range,
										   .push_constant_count = 1};
	ZHLN::Vk::PipelineLayout pipelineLayout(ctx.Device(),
											ZHLN_CreatePipelineLayout(ctx.Device(), &pLayoutDesc));

	auto vertex_bindings = ZHLN::Vk::VertexTraits<Vertex>::Bindings();
	auto vertex_attributes = ZHLN::Vk::VertexTraits<Vertex>::Attributes();

	ZHLN_GraphicsPipelineDesc pipe_desc = {
		.stages = const_cast<ZHLN_ShaderStages*>(shaders.Get()),
		.layout = pipelineLayout.Get(),
		.vertex_bindings = vertex_bindings.data(),
		.vertex_attributes = vertex_attributes.data(),
		.vertex_binding_count = static_cast<uint32_t>(vertex_bindings.size()),
		.vertex_attribute_count = static_cast<uint32_t>(vertex_attributes.size()),
		.color_format = VK_FORMAT_B8G8R8A8_SRGB,
		.depth_format = VK_FORMAT_D32_SFLOAT,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		.polygon_mode = VK_POLYGON_MODE_FILL,
		.cull_mode = VK_CULL_MODE_BACK_BIT,
		.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.depth_test = true,
		.depth_write = true,
		.blend_enable = false};
	ZHLN::Vk::Pipeline pipeline(ctx.Device(),
								ZHLN_CreateGraphicsPipeline(ctx.Device(), &pipe_desc));

	// --- 6. Render Loop Resources ---
	ZHLN::Vk::Swapchain swapchain(ctx.Device(), {});
	auto sync = ZHLN::Vk::FrameSync<3>::Create(ctx.Device());
	auto pools =
		ZHLN::Vk::CommandPools<3>::Create(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	ZHLN::Vk::SemaphorePool present_semaphores;

	ZHLN::Vk::Image depth_image;
	VkImageView depth_view = VK_NULL_HANDLE;
	bool depth_initialized = false;

	auto rebuild = [&]() {
		vkDeviceWaitIdle(ctx.Device());
		depth_initialized = false;
		ZHLN_Device raw_dev = {ctx.Device(), ctx.GraphicsQueue(), ctx.PresentQueue()};
		ZHLN_PhysicalDeviceInfo raw_phys = ctx.PhysicalInfo();
		ZHLN_SwapchainDesc s_desc = {.device = &raw_dev,
									 .physical = &raw_phys,
									 .surface = surface.Get(),
									 .width = win.width,
									 .height = win.height,
									 .vsync = true,
									 .old_swapchain = VK_NULL_HANDLE};
		swapchain.Rebuild(s_desc);
		present_semaphores.Rebuild(ctx.Device(), swapchain.Get().image_count);

		if (depth_view)
			vkDestroyImageView(ctx.Device(), depth_view, nullptr);
		VkImageCreateInfo img_info = {
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
		depth_image = ZHLN::Vk::Image::Create(allocator.Get(), img_info, VMA_MEMORY_USAGE_GPU_ONLY);
		depth_view = CreateImageView(ctx.Device(), depth_image.Handle(), VK_FORMAT_D32_SFLOAT,
									 VK_IMAGE_ASPECT_DEPTH_BIT);
		win.resized = false;
	};

	// --- 7. Main Loop ---
	uint32_t frame_index = 0;
	win.resized = true;
	auto startTime = std::chrono::high_resolution_clock::now();

	std::println("Rendering Started...");

	while (win.running) {
		ZHLN::Demo::ProcessEvents(win);
		if (win.width == 0 || win.height == 0)
			continue;
		if (win.resized)
			rebuild();

		float time =
			std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - startTime)
				.count();

		// 1. Human eye-level (1.7 is a bit low for this scale, try 2.0 or 3.0)
		float eyeHeight = 2.5f;

		// 2. The Long Axis is X in this model.
		// The atrium is roughly 40 units long in X (-20 to +20)
		float camX = std::sin(time * 0.1f) * 18.0f;
		float camZ = 0.0f; // Stay in the center of the hallway

		// 3. Direction logic: Look toward the end of the hall we are walking toward
		float lookTargetX = camX + (std::cos(time * 0.1f) > 0 ? 5.0f : -5.0f);

		Mat4 view =
			LookAt({camX, eyeHeight, camZ}, {lookTargetX, eyeHeight, 0.0f}, {0.0f, 1.0f, 0.0f});

		// 4. Projection
		Mat4 proj = Perspective(1.0472f, (float)win.width / (float)win.height, 0.1f, 1000.0f);

		Mat4 viewProj = Multiply(proj, view);
		const ZHLN_FrameSync& frame_sync = sync[frame_index];
		ZHLN_CommandPool& pool = pools[frame_index];
		VkCommandBuffer cmd = pools.Cmd(frame_index);

		ZHLN_WaitAndResetFrame(ctx.Device(), frame_sync.in_flight, &pool);

		uint32_t image_index = 0;
		ZHLN_AcquireDesc acq = {.swapchain = swapchain.Get().handle,
								.image_available = frame_sync.image_available,
								.timeout_ns = UINT64_MAX};
		if (ZHLN_AcquireImage(ctx.Device(), &acq, &image_index) == ZHLN_FrameResult_OutOfDate) {
			win.resized = true;
			continue;
		}

		ZHLN_BeginCommandBuffer(cmd);
		VkImage img = swapchain.Get().images[image_index];
		ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED,
								   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, img);

		if (!depth_initialized) {
			ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED,
									   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
				cmd, depth_image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);
			depth_initialized = true;
		}

		ZHLN_RenderPassDesc pass = {.target_view = swapchain.Get().views[image_index],
									.depth_view = depth_view,
									.extent = swapchain.Get().extent,
									.clear_color = {0.5f, 0.7f, 1.0f, 1.0f},
									.clear_depth = 1.0f}; // Sky Blue clear
		{
			ZHLN::Vk::ScopedRendering render(cmd, pass);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());

			VkBuffer vboHandle = vbo.Handle();
			VkDeviceSize offset = 0;
			vkCmdBindVertexBuffers(cmd, 0, 1, &vboHandle, &offset);
			vkCmdBindIndexBuffer(cmd, ibo.Handle(), 0, VK_INDEX_TYPE_UINT32);

			int current_mat = -2;

			// Iterate over the ASSEMBLED draw calls, not the raw primitives
			for (const auto& draw : scene_draw_calls) {

				// Calculate Node-Specific MVP: MVP = (Proj * View) * Model
				Mat4 nodeMVP = Multiply(viewProj, draw.worldMatrix);

				// Push THIS node's matrix to the GPU
				ZHLN::Vk::Push(cmd, pipelineLayout.Get(), VK_SHADER_STAGE_VERTEX_BIT, nodeMVP);

				// Bind material if it changed
				if (draw.mesh->materialIndex != current_mat) {
					current_mat = draw.mesh->materialIndex;
					VkDescriptorSet dSet = (current_mat >= 0) ? materials[current_mat].descriptorSet
															  : fallbackMat.descriptorSet;
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
											pipelineLayout.Get(), 0, 1, &dSet, 0, nullptr);
				}

				// Draw using the mesh data pointed to by this draw call
				vkCmdDrawIndexed(cmd, draw.mesh->indexCount, 1, draw.mesh->firstIndex, 0, 0);
			}
		}

		ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(cmd, img);
		ZHLN_EndCommandBuffer(cmd);

		VkCommandBufferSubmitInfo cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
											  .commandBuffer = cmd};
		VkSemaphoreSubmitInfo wait_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
										   .semaphore = frame_sync.image_available,
										   .stageMask =
											   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
		VkSemaphoreSubmitInfo signal_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
											 .semaphore = present_semaphores[image_index],
											 .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT};
		VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
								.waitSemaphoreInfoCount = 1,
								.pWaitSemaphoreInfos = &wait_info,
								.commandBufferInfoCount = 1,
								.pCommandBufferInfos = &cmd_info,
								.signalSemaphoreInfoCount = 1,
								.pSignalSemaphoreInfos = &signal_info};
		vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, frame_sync.in_flight);

		ZHLN_PresentDesc pres = {.present_queue = ctx.PresentQueue(),
								 .swapchain = swapchain.Get().handle,
								 .render_finished = present_semaphores[image_index],
								 .image_index = image_index};
		if (ZHLN_PresentFrame(&pres) != ZHLN_FrameResult_Ok)
			win.resized = true;

		frame_index = (frame_index + 1) % 3;
	}

	vkDeviceWaitIdle(ctx.Device());

	// Cleanup
	vkDestroySampler(ctx.Device(), defaultSampler, nullptr);
	vkDestroyImageView(ctx.Device(), dummyTex.view, nullptr);
	for (auto& tex : textures) {
		if (tex.view)
			vkDestroyImageView(ctx.Device(), tex.view, nullptr);
	}
	if (depth_view)
		vkDestroyImageView(ctx.Device(), depth_view, nullptr);

	vkDestroyDescriptorPool(ctx.Device(), descPool, nullptr);
	vkDestroyDescriptorSetLayout(ctx.Device(), descLayout, nullptr);

	ZHLN::Demo::DestroyWindow(win);
	return 0;
}