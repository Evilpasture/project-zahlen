#include "../cube_app/math.hpp"
#include "Allocator.hpp"
#include "DescriptorLayout.hpp"
#include "PipelineBuilder.hpp"
#include "PresentationContext.hpp"
#include "RenderCore.hpp"
#include "RenderGraph.hpp"
#include "RenderTarget.hpp"
#include "SamplerBuilder.hpp"
#include "StandardPasses.hpp"
#include "Texture.hpp"
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

ZHLN_REFLECT_VERTEX(ZHLN::Vk::Vertex, pos, norm, uv0, uv1);

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

// With bindless, a material no longer holds a descriptor set.
// It just holds the integer index into the global texture array.
struct MaterialAsset {
	uint32_t albedoIdx;
	uint32_t normalIdx;
	uint32_t pbrIdx;
	uint32_t lightmapIdx;
	uint32_t emissiveIdx;
};

struct Light {
	float position[3];
	uint32_t type; // 0=Dir, 1=Point, 2=Spot
	float color[3];
	float intensity;
	float direction[3];
	float range;
	float innerConeCos;
	float outerConeCos;
	float _pad[2];
};

struct AppFrameData {
	ZHLN::Vk::Passes::PBRMainConfig mainPass;
	ZHLN::Vk::Passes::ShadowConfig shadowPass;
	ZHLN::Vk::Passes::FXAAConfig fxaaPass;
};

// Bindless Scene Layout
using SceneLayout =
	ZHLN::Vk::DescriptorLayout<ZHLN::Vk::BindlessSampledImageSlot<0, 4096>, // Global Textures Array
							   ZHLN::Vk::SamplerSlot<1>, // Global Default Sampler

							   // Note: Shadow mapping is intentionally kept outside the bindless
							   // array because it uses a SamplerComparisonState (depth compare) and
							   // is a unique render target rather than a generic material property.
							   ZHLN::Vk::SampledImageSlot<2>, // Shadow Map
							   ZHLN::Vk::SamplerSlot<3>,	  // Shadow Sampler
							   ZHLN::Vk::SamplerSlot<4>,
							   ZHLN::Vk::StorageBufferSlot<5, VK_SHADER_STAGE_FRAGMENT_BIT>>;

using FXAALayout =
	ZHLN::Vk::DescriptorLayout<ZHLN::Vk::SampledImageSlot<0>, ZHLN::Vk::SamplerSlot<1>>;

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
					else if (attr->type == cgltf_attribute_type_texcoord) {
						if (attr->index == 0) // UV0
							cgltf_accessor_read_float(attr->data, v,
													  scene.vertices[startVert + v].uv0.data(), 2);
						else if (attr->index == 1) // UV1
							cgltf_accessor_read_float(attr->data, v,
													  scene.vertices[startVert + v].uv1.data(), 2);
					}
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

// File-based wrapper
template <VkFormat F>
static ZHLN::Vk::TextureAsset
UploadTextureFromFile(ZHLN::Vk::Allocator& allocator, const ZHLN::Vk::Context& ctx,
					  const VkImageCreateInfo& baseInfo, const std::string& fullPath) {
	int tw, th, tc;
	// Force 4 channels (RGBA) to match our buffer size logic
	stbi_uc* pixels = stbi_load(fullPath.c_str(), &tw, &th, &tc, STBI_rgb_alpha);
	if (!pixels) {
		std::println(stderr, "Failed to load: {}", fullPath);
		return {};
	}

	VkImageCreateInfo info = baseInfo;
	info.extent = {static_cast<uint32_t>(tw), static_cast<uint32_t>(th), 1};

	// Specify <F> here!
	ZHLN::Vk::TextureAsset result = ZHLN::Vk::UploadTexture<F>(allocator, ctx, info, pixels);

	stbi_image_free(pixels);

	// We return result; C++ handles the move automatically here
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
// Frame recording
// ============================================================================

static void RecordFrame(VkCommandBuffer cmd, ZHLN::Vk::GraphImage& swapchainRes,
						ZHLN::Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>& sceneColor,
						ZHLN::Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>& shadowMap,
						ZHLN::Vk::GraphImage& depthTracker, const AppFrameData& d) {
	using namespace ZHLN::Vk;
	RenderGraph Graph;

	// 1. Shadows
	Passes::AddShadowPass(Graph, shadowMap.tracker, d.shadowPass);

	// 2. PBR Scene
	Passes::AddPBRMainPass(Graph, sceneColor.tracker, depthTracker, shadowMap.tracker, d.mainPass);

	// 3. FXAA
	Passes::AddFXAAPass(Graph, sceneColor.tracker, swapchainRes, d.fxaaPass);

	// 4. Final Handoff
	Graph.AddPass("Handoff").Present(swapchainRes);

	Graph.Execute(cmd);
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

	std::vector<Light> sceneLights;

	for (cgltf_size i = 0; i < gltf->nodes_count; ++i) {
		cgltf_node* node = &gltf->nodes[i];
		if (!node->light)
			continue;

		float matrix[16];
		cgltf_node_transform_world(node, matrix);

		Light l{};
		l.type = (uint32_t)node->light->type; // Matches cgltf_light_type
		l.intensity = node->light->intensity;
		l.range = node->light->range;
		l.color[0] = node->light->color[0];
		l.color[1] = node->light->color[1];
		l.color[2] = node->light->color[2];

		// Extract Position from world matrix
		l.position[0] = matrix[12];
		l.position[1] = matrix[13];
		l.position[2] = matrix[14];

		// Extract Direction (Forward vector is -Z in glTF)
		l.direction[0] = -matrix[8];
		l.direction[1] = -matrix[9];
		l.direction[2] = -matrix[10];

		if (node->light->type == cgltf_light_type_spot) {
			l.innerConeCos = std::cos(node->light->spot_inner_cone_angle);
			l.outerConeCos = std::cos(node->light->spot_outer_cone_angle);
		}

		sceneLights.push_back(l);
	}

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

	auto lightBuffer = ZHLN::Vk::Buffer::Create(allocator.Get(), sceneLights.size() * sizeof(Light),
												VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
													VK_BUFFER_USAGE_TRANSFER_DST_BIT,
												VMA_MEMORY_USAGE_GPU_ONLY);

	auto stagingLights =
		ZHLN::Vk::UploadToBuffer(allocator.Get(), setupCmd, lightBuffer, sceneLights.data(),
								 sceneLights.size() * sizeof(Light));

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
	VkImageCreateInfo dummyInfo = texBaseInfo;
	dummyInfo.extent = {1, 1, 1};

	ZHLN::Vk::TextureAsset dummyTex =
		ZHLN::Vk::UploadTexture<VK_FORMAT_R8G8B8A8_UNORM>(allocator, ctx, dummyInfo, &white_pixel);

	uint32_t black_pixel = 0xFF000000; // Alpha 1.0, RGB 0.0
	ZHLN::Vk::TextureAsset blackTex =
		ZHLN::Vk::UploadTexture<VK_FORMAT_R8G8B8A8_UNORM>(allocator, ctx, dummyInfo, &black_pixel);

	// --- Textures Loop ---
	std::vector<ZHLN::Vk::TextureAsset> textures(gltf->images_count);
	std::println("Uploading {} textures...", gltf->images_count);
	for (cgltf_size i = 0; i < gltf->images_count; ++i) {
		ZHLN::Demo::ProcessEvents(win);
		std::string fullPath = paths.asset_prefix + gltf->images[i].uri;

		// Sponza uses "BaseColor" in the filenames for Albedo maps
		if (fullPath.find("BaseColor") != std::string::npos) {
			textures[i] = UploadTextureFromFile<VK_FORMAT_R8G8B8A8_SRGB>(allocator, ctx,
																		 texBaseInfo, fullPath);
		} else if (fullPath.find("Lightmap") != std::string::npos) {
			// Lightmaps should ALWAYS be UNORM (they are data)
			textures[i] = UploadTextureFromFile<VK_FORMAT_R8G8B8A8_UNORM>(allocator, ctx,
																		  texBaseInfo, fullPath);
		} else {
			// Normals and Roughness/Metallic maps must be Linear (UNORM)
			textures[i] = UploadTextureFromFile<VK_FORMAT_R8G8B8A8_UNORM>(allocator, ctx,
																		  texBaseInfo, fullPath);
		}
		std::println("  [{}/{}] {}", i + 1, gltf->images_count, gltf->images[i].uri);
	}
	// --- Infrastructure Setup ---
	ZHLN::Vk::PresentationContext presentation;
	if (!presentation.Init(ctx, allocator, surface.Get(), win.width, win.height)) {
		std::println(stderr, "Failed to init Presentation Context.");
		return -1;
	}

	auto sceneColor = ZHLN::Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
		allocator, ctx, presentation.swapchain.Get().extent,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

	// --- Technique-Specific Targets ---
	constexpr uint32_t SHADOW_RES = 4096;
	auto shadowMap = ZHLN::Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
		allocator, ctx, {SHADOW_RES, SHADOW_RES},
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

	VkExtent2D shadowExtent = {SHADOW_RES, SHADOW_RES};

	// --- Samplers (Via Builder) ---
	auto defaultSampler =
		ZHLN::Vk::SamplerBuilder{}.Linear().Repeat().Anisotropy(8.0f).Build(ctx.Device());
	auto shadowSampler = ZHLN::Vk::SamplerBuilder{}
							 .Linear()
							 .ClampToBorder(VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE)
							 .DepthCompare()
							 .Build(ctx.Device());
	auto lightmapSampler = ZHLN::Vk::SamplerBuilder{}
							   .Linear()
							   .ClampToEdge() // Lightmaps MUST NOT wrap/repeat
							   .Build(ctx.Device());

	// --- Global Descriptor Set Layout & Allocation ---
	auto descLayout = SceneLayout::CreateLayout(ctx.Device());
	auto descPool = SceneLayout::CreatePool(ctx.Device(), 1);

	VkDescriptorSet globalSet =
		SceneLayout::Allocate(ctx.Device(), descPool.Get(), descLayout.Get());

	SceneLayout::Write(
		ctx.Device(), globalSet, ZHLN::Vk::SkipWrite{},
		ZHLN::Vk::SamplerWrite{defaultSampler.Get()}, ZHLN::Vk::ImageWrite{shadowMap.view.Get()},
		ZHLN::Vk::SamplerWrite{shadowSampler.Get()}, ZHLN::Vk::SamplerWrite{lightmapSampler.Get()},
		ZHLN::Vk::BufferWrite{lightBuffer.Handle()});

	// 1. Define the Registry tied to the Layout and specific Binding Index (0)
	ZHLN::Vk::BindlessRegistry<SceneLayout, 0> bindless;

	// 2. Initialize (Binding index and capacity are deduced from the template)
	bindless.Init(ctx.Device(), globalSet);

	// 3. Registering remains runtime-dynamic but type-safe
	uint32_t fallbackTexIdx = bindless.RegisterImage(dummyTex.view.Get());
	uint32_t blackTexIdx = bindless.RegisterImage(blackTex.view.Get());

	std::println("Bindless Registry Initialized. Capacity: {}", bindless.Capacity());

	// Register loaded gltf images
	std::vector<uint32_t> bindlessTextureIndices(gltf->images_count, fallbackTexIdx);
	for (cgltf_size i = 0; i < gltf->images_count; ++i) {
		// FIX: Only register if the view is valid!
		if (textures[i].view.Valid()) {
			bindlessTextureIndices[i] = bindless.RegisterImage(textures[i].view.Get());
		} else {
			// If it failed to load, point this image index to the dummy white texture
			bindlessTextureIndices[i] = fallbackTexIdx;
		}
	}

	// Setup materials to point to correct bindless index
	std::vector<MaterialAsset> materials(gltf->materials_count);
	for (cgltf_size i = 0; i < gltf->materials_count; ++i) {
		cgltf_material* mat = &gltf->materials[i];

		// Default
		materials[i].albedoIdx = fallbackTexIdx;
		materials[i].normalIdx = fallbackTexIdx;
		materials[i].pbrIdx = fallbackTexIdx;
		materials[i].lightmapIdx = fallbackTexIdx;
		materials[i].emissiveIdx = blackTexIdx;

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
		// Intel Sponza often names lightmaps similarly to the material.
		// We search the texture array for anything with "Lightmap" and the material's name.
		for (uint32_t t = 0; t < gltf->images_count; ++t) {
			std::string imgName = gltf->images[t].uri;
			// Check if this image is a lightmap
			if (imgName.find("Lightmap") != std::string::npos) {
				// Basic heuristic: many Sponza versions map 1:1 by index or name
				// For now, let's assume the first lightmap found is the global one
				// or check for material name matches.
				materials[i].lightmapIdx = bindlessTextureIndices[t];
				break;
			}
		}
		// Standard GLTF Emissive slot
		if (mat->emissive_texture.texture && mat->emissive_texture.texture->image) {
			int imgIdx = static_cast<int>(mat->emissive_texture.texture->image - gltf->images);
			materials[i].emissiveIdx = bindlessTextureIndices[imgIdx];
		}
	}
	MaterialAsset fallbackMat = {.albedoIdx = fallbackTexIdx, .normalIdx = fallbackTexIdx};

	// --- Post processing ---

	// --- 1. FXAA Descriptor Setup ---

	auto fxaaDescLayout = FXAALayout::CreateLayout(ctx.Device());
	auto fxaaDescPool = FXAALayout::CreatePool(ctx.Device(), 1);
	VkDescriptorSet fxaaSet =
		FXAALayout::Allocate(ctx.Device(), fxaaDescPool.Get(), fxaaDescLayout.Get());

	// --- 2. FXAA Pipeline Layout ---
	VkDescriptorSetLayout rawFxaaLayout = fxaaDescLayout.Get();
	ZHLN_PipelineLayoutDesc fxaaLayoutDesc = {.set_layouts = &rawFxaaLayout,
											  .set_layout_count = 1,
											  .push_constants = nullptr,
											  .push_constant_count = 0};
	ZHLN::Vk::PipelineLayout fxaaPipelineLayout(
		ctx.Device(), ZHLN_CreatePipelineLayout(ctx.Device(), &fxaaLayoutDesc));

	// --- 3. FXAA Shader and Pipeline ---
	auto fxaaShaders = ZHLN::Vk::ShaderStages::FromFiles(
		ctx.Device(), "fxaa.hlsl.VSMain.spv", "fxaa.hlsl.PSMain.spv", "VSMain", "PSMain");

	auto fxaaPipeline = ZHLN::Vk::PipelineBuilder{}
							.Shaders(fxaaShaders)
							.Layout(fxaaPipelineLayout.Get())
							.ColorFormat(VK_FORMAT_B8G8R8A8_SRGB) // Output to swapchain
							.NoDepth()
							.CullNone()
							.Build(ctx.Device());

	// --- Pipelines (Via Factory & Builder) ---
	auto shaders = ZHLN::Vk::ShaderStages::FromFiles(ctx.Device(), paths.vert_spv, paths.frag_spv,
													 "VSMain", "PSMain");
	auto shadowShaders = ZHLN::Vk::ShaderStages::FromFiles(
		ctx.Device(), paths.shadow_vert_spv, paths.shadow_frag_spv, "VSMain", "PSMain");

	// Note: mainPush now needs FRAGMENT bit to read textureIndex
	VkPushConstantRange mainPush = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
									sizeof(ZHLN::Vk::Passes::PBRPushConstants)};
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
						.ColorFormat(VK_FORMAT_R16G16B16A16_SFLOAT)
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

	// --- Frame loop resources ---
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

		if (win.resized) {
			if (!presentation.Rebuild(win.width, win.height))
				continue;

			// FIX: Change R8G8B8A8 to B8G8R8A8 to match the variable type
			sceneColor = ZHLN::Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
				allocator, ctx, presentation.swapchain.Get().extent,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

			// Update the FXAA descriptor set to point to the new image
			FXAALayout::Write(ctx.Device(), fxaaSet, ZHLN::Vk::ImageWrite{sceneColor.view.Get()},
							  ZHLN::Vk::SamplerWrite{defaultSampler.Get()});

			win.resized = false;
		}

		if (!presentation.swapchain.Valid() || presentation.swapchain.Get().extent.width == 0)
			continue;

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
		ZHLN_AcquireDesc acq = {.swapchain = presentation.swapchain.Get().handle,
								.image_available = frame_sync.image_available,
								.timeout_ns = UINT64_MAX};
		if (ZHLN_AcquireImage(ctx.Device(), &acq, &image_index) == ZHLN_FrameResult_OutOfDate) {
			win.resized = true;
			continue;
		}

		ZHLN_BeginCommandBuffer(cmd);

		// Transient tracker for this specific frame's swapchain image
		ZHLN::Vk::GraphImage swapchainRes = ZHLN::Vk::GraphImage::Create(
			presentation.swapchain.Get().images[image_index],
			presentation.swapchain.Get().views[image_index], presentation.swapchain.Get().extent,
			VK_IMAGE_ASPECT_COLOR_BIT);

		// 1. Prepare Draw Call Data for this frame
		std::vector<ZHLN::Vk::Passes::PBRDrawCall> pbrCalls;
		pbrCalls.reserve(scene.drawCalls.size());

		for (const auto& dc : scene.drawCalls) {
			const auto& mat =
				(dc.mesh->materialIndex >= 0) ? materials[dc.mesh->materialIndex] : fallbackMat;
			pbrCalls.push_back({.worldMatrix = dc.worldMatrix.data, // Map Mat4 to std::array
								.albedoIdx = mat.albedoIdx,
								.normalIdx = mat.normalIdx,
								.pbrIdx = mat.pbrIdx,
								.lightmapIdx = mat.lightmapIdx,
								.emissiveIdx = mat.emissiveIdx,
								.indexCount = dc.mesh->indexCount,
								.firstIndex = dc.mesh->firstIndex});
		}

		std::array<float, 3> lightPos = {40.f, 60.f, 40.f};
		std::array<float, 3> lightTarget = {0.f, 10.f, 0.f};

		// Vector = Target - Position
		float dirX = lightTarget[0] - lightPos[0];
		float dirY = lightTarget[1] - lightPos[1];
		float dirZ = lightTarget[2] - lightPos[2];
		float len = std::sqrt(dirX * dirX + dirY * dirY + dirZ * dirZ);

		// 2. Pack the Scene Context
		ZHLN::Vk::Passes::PBRSceneContext sceneCtx = {
			.viewProj = viewProj.data,
			.lightSpaceMatrix = lightSpace.data,
			.camPos = {camX, camY, camZ, 1.0f},
			.lightDir = {dirX / len, dirY / len, dirZ / len, 0.0f}, // Set your sun direction
			.lightCount = static_cast<uint32_t>(sceneLights.size()),
			.globalSet = globalSet,
			.vbo = vbo.Handle(),
			.ibo = ibo.Handle()};

		// 3. Define the Pass Configurations
		AppFrameData frameData = {
			.mainPass = {pipeline.Get(), pipelineLayout.Get(), &pbrCalls, &sceneCtx},
			.shadowPass = {shadowPipeline.Get(), shadowLayout.Get(), &pbrCalls, &sceneCtx},
			.fxaaPass = {fxaaPipeline.Get(), fxaaPipelineLayout.Get(), fxaaSet}};

		// 4. Execute (RecordFrame now uses the backend presets)
		RecordFrame(cmd, swapchainRes, sceneColor, shadowMap, presentation.depthTarget.tracker,
					frameData);

		ZHLN_EndCommandBuffer(cmd);

		ZHLN_FrameSubmitDesc submitDesc = {.graphicsQueue = ctx.GraphicsQueue(),
										   .presentQueue = ctx.PresentQueue(),
										   .cmd = cmd,
										   .imageAvailable = frame_sync.image_available,
										   .renderFinished =
											   presentation.presentSemaphores[image_index],
										   .inFlight = frame_sync.in_flight,
										   .swapchain = presentation.swapchain.Get().handle,
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