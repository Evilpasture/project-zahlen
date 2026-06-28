#include "RenderCore.hpp"
#include "RenderInternal.hpp"
#include "Resources.hpp"

#include <cstdint>
namespace ZHLN {

void RenderContext::Impl::BuildProceduralBakePipeline() {
	proceduralBakeDescLayout = BakeLayout::CreateLayout(ctx.Device());
	proceduralBakeDescPool = BakeLayout::CreatePool(ctx.Device(), 1); // 1 pool is sufficient

	proceduralBakeSet = BakeLayout::Allocate(ctx.Device(), proceduralBakeDescPool.Get(),
											 proceduralBakeDescLayout.Get());

	VkPushConstantRange push = {
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		.offset = 0,
		.size = sizeof(uint32_t) * 2 + sizeof(float) * 2 // width, height, scale, randomness
	};

	const void* cs_code = nullptr;
	size_t cs_size = 0;
	std::vector<uint32_t> disk_cs;

	LoadShaderData({.path = SHADER_PROCEDURAL_BAKE_CS_PATH,
					.fallbackCode = ZHLN_Resource_ProceduralBakeCompSpv,
					.fallbackSize = ZHLN_Resource_ProceduralBakeCompSpv_Len,
					.entryPoint = "CSMain"},
				   cs_code, cs_size, disk_cs);

	ZHLN_ShaderDesc shader = {
		.code = Vk::AsSpirV(cs_code), .size = cs_size, .entry_point = "CSMain"};

	// Map specialization indices to driver pipeline branches
	std::array<VkSpecializationMapEntry, 1> specEntries = {
		{{.constantID = 0, .offset = 0, .size = sizeof(int)}}};

	std::array<int, 3> variants = {0, 1, 2}; // 0=Voronoi, 1=Perlin, 2=Wave
	std::array<VkSpecializationInfo, 3> specInfos{};
	for (int i = 0; i < 3; ++i) {
		specInfos[i] = {.mapEntryCount = 1,
						.pMapEntries = specEntries.data(),
						.dataSize = sizeof(int),
						.pData = &variants[i]};
	}

	// Compile the three driver-optimized variants using our new framework API!
	if (proceduralBakePass.BuildVariants(ctx.Device(), proceduralBakeDescLayout.Get(), shader,
										 specInfos, &push, 1)) {
		ZHLN::Log("[Shader] GPU Procedural Bake Compute Pipeline initialized with specialization "
				  "variants.");
	} else {
		ZHLN::Panic("FATAL: Failed to build specialized Procedural Bake Compute variants!");
	}
}

uint32_t RenderContext::Impl::BakeProceduralTexture(uint32_t width, uint32_t height,
													uint32_t variantIdx, float scale,
													float randomness, float distortion) {
	const VkDevice device = ctx.Device();

	// 1. Create a texture with STORAGE and SAMPLED usage
	const VkImageCreateInfo imgInfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
									   .imageType = VK_IMAGE_TYPE_2D,
									   .format = VK_FORMAT_R8G8B8A8_UNORM,
									   .extent = {.width = width, .height = height, .depth = 1},
									   .mipLevels = 1,
									   .arrayLayers = 1,
									   .samples = VK_SAMPLE_COUNT_1_BIT,
									   .tiling = VK_IMAGE_TILING_OPTIMAL,
									   .usage =
										   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
									   .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
									   .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};

	auto gpuImage = Vk::Image::Create(allocator.Get(), imgInfo, VMA_MEMORY_USAGE_GPU_ONLY);
	auto writeView = Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(device, gpuImage.Handle(),
															  VK_IMAGE_ASPECT_COLOR_BIT, 1);

	// Write to compute descriptor set
	BakeLayout::Write(device, proceduralBakeSet, Vk::ImageWrite{.view = writeView.Get()});

	// 2. Dispatch the Compute Shader
	Vk::CommandPool tempPool(device, ctx.PhysicalInfo().graphics_family);

	// FIX: Handle [[nodiscard]] return value safely
	if (!tempPool.Allocate(1)) {
		ZHLN::Panic("FATAL: Failed to allocate temporary command pool for procedural baking!");
		return 0;
	}
	VkCommandBuffer cmd = tempPool[0];

	ZHLN_BeginCommandBuffer(cmd);

	// Transition Undefined -> General (Safe for Compute storage writes)
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd,
																			 gpuImage.Handle());

	struct BakePush {
		uint32_t width;
		uint32_t height;
		float scale;
		float randomness;
		float distortion;
		uint32_t bakeType;
	} pc = {.width = width,
			.height = height,
			.scale = scale,
			.randomness = randomness,
			.distortion = distortion,
			.bakeType = variantIdx};

	proceduralBakePass.Dispatch(cmd, proceduralBakeSet, (width + 15) / 16, (height + 15) / 16, 1,
								pc);

	// Transition General -> Shader Read Only (Ready for Bindless fragment reads)
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
		cmd, gpuImage.Handle());

	ZHLN_EndCommandBuffer(cmd);

	// Submit queue synchronously
	VkCommandBufferSubmitInfo subInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
										 .commandBuffer = cmd};
	VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
							.commandBufferInfoCount = 1,
							.pCommandBufferInfos = &subInfo};
	vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(ctx.GraphicsQueue());

	// 3. Register our generated view into the Bindless Set
	uint32_t index = nextTextureIndex++;
	VkDescriptorImageInfo bindlessUpdate = {.sampler = VK_NULL_HANDLE,
											.imageView = writeView.Get(),
											.imageLayout =
												VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

	std::array<VkWriteDescriptorSet, 2> writes = {};
	for (int i = 0; i < 2; ++i) {
		writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					 .dstSet = bindlessSets[i],
					 .dstBinding = 0,
					 .dstArrayElement = index,
					 .descriptorCount = 1,
					 .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
					 .pImageInfo = &bindlessUpdate};
	}
	vkUpdateDescriptorSets(device, 2, writes.data(), 0, nullptr);

	textureImages.push_back(std::move(gpuImage));
	textureViews.push_back(std::move(writeView));

	return index;
}

} // namespace ZHLN
