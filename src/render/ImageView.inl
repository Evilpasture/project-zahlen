#pragma once
#include "ImageView.hpp"

namespace ZHLN::Vk {

// ============================================================================
// Image View Helpers Implementation
// ============================================================================
namespace {
struct FormatAspectMapping {
	VkFormat format;
	VkImageAspectFlags aspect;
};
} // namespace

inline constexpr std::array<FormatAspectMapping, 10> kFormatAspectTable = {
	{{.format = VK_FORMAT_R16G16B16A16_SFLOAT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
	 {.format = VK_FORMAT_R32G32B32A32_SFLOAT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
	 {.format = VK_FORMAT_R8G8B8A8_UNORM, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
	 {.format = VK_FORMAT_R8G8B8A8_SRGB, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
	 {.format = VK_FORMAT_B8G8R8A8_SRGB, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
	 {.format = VK_FORMAT_R8G8_UNORM, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
	 {.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
	 {.format = VK_FORMAT_D32_SFLOAT, .aspect = VK_IMAGE_ASPECT_DEPTH_BIT},
	 {.format = VK_FORMAT_D24_UNORM_S8_UINT,
	  .aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT},
	 {.format = VK_FORMAT_R16G16_SFLOAT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT}}};

constexpr auto GetFormatAspect(VkFormat format) noexcept -> VkImageAspectFlags {
	for (const auto& mapping : kFormatAspectTable) {
		if (mapping.format == format) {
			return mapping.aspect;
		}
	}
	return VK_IMAGE_ASPECT_NONE;
}

template <VkFormat F>
inline auto CreateView(VkDevice device, VkImage image, VkImageAspectFlags aspect, uint32_t mips)
	-> ImageView {
	ZHLN_ImageViewDesc desc = {
		.image = image,
		.format = F,
		.aspect = aspect,
		.mip_levels = mips,
		.array_layers = 1,
		.view_type = VK_IMAGE_VIEW_TYPE_2D,
		.base_array_layer = 0,
	};
	VkImageView view = ZHLN_CreateImageView(device, &desc);
	return ImageView{device, view};
}

template <VkFormat F>
inline auto CreateViewCube(VkDevice device, VkImage image, uint32_t mips) -> ImageView {
	ZHLN_ImageViewDesc desc = {
		.image = image,
		.format = F,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
		.mip_levels = mips,
		.array_layers = 6,
		.view_type = VK_IMAGE_VIEW_TYPE_CUBE,
		.base_array_layer = 0,
	};
	VkImageView view = ZHLN_CreateImageView(device, &desc);
	return ImageView{device, view};
}

template <VkFormat F>
inline auto CreateView2DArray(VkDevice device, VkImage image, uint32_t baseLayer,
							  uint32_t layerCount, VkImageAspectFlags aspect, uint32_t mips)
	-> ImageView {
	ZHLN_ImageViewDesc desc = {.image = image,
							   .format = F,
							   .aspect = aspect,
							   .mip_levels = mips,
							   .array_layers = layerCount,
							   .view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
							   .base_array_layer = baseLayer};
	VkImageView view = ZHLN_CreateImageView(device, &desc);
	return ImageView{device, view};
}

template <VkFormat F>
inline auto CreateViewCubeArray(VkDevice device, VkImage image, uint32_t arrayLayers,
								VkImageAspectFlags aspect, uint32_t mips) -> ImageView {
	ZHLN_ImageViewDesc desc = {.image = image,
							   .format = F,
							   .aspect = aspect,
							   .mip_levels = mips,
							   .array_layers = arrayLayers,
							   .view_type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,
							   .base_array_layer = 0};
	VkImageView view = ZHLN_CreateImageView(device, &desc);
	return ImageView{device, view};
}
} // namespace ZHLN::Vk
