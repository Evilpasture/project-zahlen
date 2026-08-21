#pragma once
#include "ImageView.hpp"

namespace ZHLN::Vk {

// ============================================================================
// Image View Helpers Implementation
// ============================================================================
namespace {
struct FormatAspectMapping {
    VkFormat           format;
    VkImageAspectFlags aspect;
};
} // namespace

inline constexpr std::array<FormatAspectMapping, 12> kFormatAspectTable = {
    {{.format = VK_FORMAT_R16G16B16A16_SFLOAT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
     {.format = VK_FORMAT_R32G32B32A32_SFLOAT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
     {.format = VK_FORMAT_R32_SFLOAT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
     {.format = VK_FORMAT_R8G8B8A8_UNORM, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
     {.format = VK_FORMAT_R8G8B8A8_SRGB, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
     {.format = VK_FORMAT_B8G8R8A8_SRGB, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
     {.format = VK_FORMAT_R8G8_UNORM, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
     {.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
     {.format = VK_FORMAT_D32_SFLOAT, .aspect = VK_IMAGE_ASPECT_DEPTH_BIT},
     {.format = VK_FORMAT_D32_SFLOAT_S8_UINT, .aspect = VK_IMAGE_ASPECT_DEPTH_BIT},
     {.format = VK_FORMAT_D24_UNORM_S8_UINT, .aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT},
     {.format = VK_FORMAT_R16G16_SFLOAT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT}}
};

constexpr auto GetFormatAspect(VkFormat format) noexcept -> VkImageAspectFlags {
    for (const auto& mapping: kFormatAspectTable) {
        if (mapping.format == format) {
            return mapping.aspect;
        }
    }
    return VK_IMAGE_ASPECT_NONE;
}

template <VkFormat F>
inline auto CreateView(VkDevice device, VkImage image, VkImageAspectFlags aspect, uint32_t mips) -> ImageView {
    ZHLN_ImageViewDesc desc = {
        .image            = image,
        .format           = F,
        .aspect           = aspect,
        .mip_levels       = mips,
        .array_layers     = 1,
        .view_type        = VK_IMAGE_VIEW_TYPE_2D,
        .base_array_layer = 0,
        .base_mip         = {},
    };
    VkImageView view = ZHLN_CreateImageView(device, &desc);
    return ImageView {device, view};
}

template <VkFormat F>
inline auto CreateView3D(VkDevice device, VkImage image, VkImageAspectFlags aspect, uint32_t mips) -> ImageView {
    ZHLN_ImageViewDesc desc = {
        .image            = image,
        .format           = F,
        .aspect           = aspect,
        .mip_levels       = mips,
        .array_layers     = 1,
        .view_type        = VK_IMAGE_VIEW_TYPE_3D, // Standard 3D Type
        .base_array_layer = 0,
        .base_mip         = {},
    };
    VkImageView view = ZHLN_CreateImageView(device, &desc);
    return ImageView {device, view};
}

template <VkFormat F>
inline auto CreateViewCube(VkDevice device, VkImage image, uint32_t mips) -> ImageView {
    ZHLN_ImageViewDesc desc = {
        .image            = image,
        .format           = F,
        .aspect           = VK_IMAGE_ASPECT_COLOR_BIT,
        .mip_levels       = mips,
        .array_layers     = 6,
        .view_type        = VK_IMAGE_VIEW_TYPE_CUBE,
        .base_array_layer = 0,
        .base_mip         = {},
    };
    VkImageView view = ZHLN_CreateImageView(device, &desc);
    return ImageView {device, view};
}

template <VkFormat F>
inline auto CreateView2DArray(VkDevice device, VkImage image, uint32_t baseLayer, uint32_t layerCount, VkImageAspectFlags aspect, uint32_t mips) -> ImageView {
    ZHLN_ImageViewDesc desc = {
        .image            = image,
        .format           = F,
        .aspect           = aspect,
        .mip_levels       = mips,
        .array_layers     = layerCount,
        .view_type        = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .base_array_layer = baseLayer,
        .base_mip         = {},
    };
    VkImageView view = ZHLN_CreateImageView(device, &desc);
    return ImageView {device, view};
}

template <VkFormat F>
inline auto CreateViewCubeArray(VkDevice device, VkImage image, uint32_t arrayLayers, VkImageAspectFlags aspect, uint32_t mips) -> ImageView {
    ZHLN_ImageViewDesc desc = {
        .image            = image,
        .format           = F,
        .aspect           = aspect,
        .mip_levels       = mips,
        .array_layers     = arrayLayers,
        .view_type        = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,
        .base_array_layer = 0,
        .base_mip         = {},
    };
    VkImageView view = ZHLN_CreateImageView(device, &desc);
    return ImageView {device, view};
}

template <VkFormat F>
inline auto CreateViewSingleMip(VkDevice device, VkImage image, uint32_t baseMip, VkImageAspectFlags aspect) -> ImageView {
    ZHLN_ImageViewDesc desc = {
        .image            = image,
        .format           = F,
        .aspect           = aspect,
        .mip_levels       = 1,
        .array_layers     = 1,
        .view_type        = VK_IMAGE_VIEW_TYPE_2D,
        .base_array_layer = 0,
        .base_mip         = baseMip
    };
    VkImageView view = ZHLN_CreateImageView(device, &desc);
    return ImageView {device, view};
}

// ============================================================================
// VK_EXT_descriptor_heap: view-create-info helpers
// ============================================================================
// vkWriteResourceDescriptorsEXT consumes VkImageViewCreateInfo (not view
// handles), so descriptor-heap image writes carry the same parameters used to
// create the matching VkImageView above.

inline auto MakeViewCreateInfo2D(VkImage image, VkFormat format, uint32_t mipLevels, VkImageAspectFlags aspect) noexcept -> VkImageViewCreateInfo {
    return {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = 0,
        .image            = image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = format,
        .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {.aspectMask = aspect, .baseMipLevel = 0, .levelCount = mipLevels, .baseArrayLayer = 0, .layerCount = 1},
    };
}

inline auto MakeViewCreateInfoCube(VkImage image, VkFormat format, uint32_t mipLevels) noexcept -> VkImageViewCreateInfo {
    return {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = 0,
        .image            = image,
        .viewType         = VK_IMAGE_VIEW_TYPE_CUBE,
        .format           = format,
        .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = mipLevels, .baseArrayLayer = 0, .layerCount = 6},
    };
}

inline auto MakeViewCreateInfo2DArray(VkImage image, VkFormat format, uint32_t baseLayer, uint32_t layerCount, VkImageAspectFlags aspect, uint32_t mipLevels) noexcept
    -> VkImageViewCreateInfo {
    return {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = 0,
        .image            = image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .format           = format,
        .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {.aspectMask = aspect, .baseMipLevel = 0, .levelCount = mipLevels, .baseArrayLayer = baseLayer, .layerCount = layerCount},
    };
}
} // namespace ZHLN::Vk
