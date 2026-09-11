#pragma once
#include "ImageView.hpp"

namespace ZHLN::Vk {

// ============================================================================
// Image View Helpers Implementation
// ============================================================================
// Image-view creation failures for the ImageView subsystem. Kept in the named
// namespace (not the file-local block below) so callers across the renderer
// can branch on it via the type-erased Error.
enum class ImageViewCreationError : uint8_t {
    OutOfHostMemory ZHLN_ANNOTATION(ZHLN::Description<"Out of host memory">{}) = 1,
    OutOfDeviceMemory ZHLN_ANNOTATION(ZHLN::Description<"Out of device memory">{}),
    CreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Image view creation failed">{}),
};

namespace {
struct FormatAspectMapping {
    VkFormat           format;
    VkImageAspectFlags aspect;
};

constexpr auto MapImageViewError(VkResult res) noexcept -> ImageViewCreationError {
    using enum ImageViewCreationError;
    switch (res) {
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return OutOfHostMemory;
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return OutOfDeviceMemory;
        default:
            return CreationFailed;
    }
}
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

constexpr auto MakeViewCreateInfo(
    VkImage            image,
    VkFormat           format,
    VkImageViewType    viewType,
    VkImageAspectFlags aspect,
    uint32_t           mipLevels,
    uint32_t           arrayLayers,
    uint32_t           baseMip,
    uint32_t           baseLayer
) noexcept -> VkImageViewCreateInfo {
    return {
        .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image      = image,
        .viewType   = viewType,
        .format     = format,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange =
            {.aspectMask     = aspect,
             .baseMipLevel   = baseMip,
             .levelCount     = mipLevels != 0 ? mipLevels : 1,
             .baseArrayLayer = baseLayer,
             .layerCount     = arrayLayers != 0 ? arrayLayers : 1},
    };
}

constexpr auto MakeViewCreateInfo2D(
    VkImage image, VkFormat format, uint32_t mipLevels, VkImageAspectFlags aspect, uint32_t baseMip
) noexcept -> VkImageViewCreateInfo {
    return MakeViewCreateInfo(image, format, VK_IMAGE_VIEW_TYPE_2D, aspect, mipLevels, 1, baseMip, 0);
}

constexpr auto MakeViewCreateInfo3D(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mipLevels) noexcept -> VkImageViewCreateInfo {
    return MakeViewCreateInfo(image, format, VK_IMAGE_VIEW_TYPE_3D, aspect, mipLevels, 1);
}

constexpr auto MakeViewCreateInfoCube(VkImage image, VkFormat format, uint32_t mipLevels) noexcept -> VkImageViewCreateInfo {
    return MakeViewCreateInfo(image, format, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_ASPECT_COLOR_BIT, mipLevels, 6);
}

constexpr auto MakeViewCreateInfo2DArray(
    VkImage            image,
    VkFormat           format,
    uint32_t           baseLayer,
    uint32_t           layerCount,
    VkImageAspectFlags aspect,
    uint32_t           mipLevels,
    uint32_t           baseMip
) noexcept -> VkImageViewCreateInfo {
    return MakeViewCreateInfo(image, format, VK_IMAGE_VIEW_TYPE_2D_ARRAY, aspect, mipLevels, layerCount, baseMip, baseLayer);
}

constexpr auto MakeViewCreateInfoCubeArray(
    VkImage image, VkFormat format, uint32_t arrayLayers, VkImageAspectFlags aspect, uint32_t mipLevels
) noexcept -> VkImageViewCreateInfo {
    return MakeViewCreateInfo(image, format, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, aspect, mipLevels, arrayLayers);
}

inline auto CreateView(VkDevice device, const VkImageViewCreateInfo& info) -> std::expected<ImageView, Error> {
    VkImageView    view = VK_NULL_HANDLE;
    const VkResult res  = vkCreateImageView(device, &info, nullptr, &view);
    if (res != VK_SUCCESS) {
        return std::unexpected(MapImageViewError(res));
    }
    return ImageView {device, view};
}

inline auto CreateView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mips) -> std::expected<ImageView, Error> {
    return CreateView(device, MakeViewCreateInfo2D(image, format, mips, aspect));
}

template <VkFormat F>
inline auto CreateView(VkDevice device, VkImage image, VkImageAspectFlags aspect, uint32_t mips) -> std::expected<ImageView, Error> {
    return CreateView(device, image, F, aspect, mips);
}

template <VkFormat F>
inline auto CreateView3D(VkDevice device, VkImage image, VkImageAspectFlags aspect, uint32_t mips) -> std::expected<ImageView, Error> {
    return CreateView(device, MakeViewCreateInfo3D(image, F, aspect, mips));
}

template <VkFormat F>
inline auto CreateViewCube(VkDevice device, VkImage image, uint32_t mips) -> std::expected<ImageView, Error> {
    return CreateView(device, MakeViewCreateInfoCube(image, F, mips));
}

template <VkFormat F>
inline auto CreateView2DArray(VkDevice device, VkImage image, uint32_t baseLayer, uint32_t layerCount, VkImageAspectFlags aspect, uint32_t mips)
    -> std::expected<ImageView, Error> {
    return CreateView(device, MakeViewCreateInfo2DArray(image, F, baseLayer, layerCount, aspect, mips));
}

template <VkFormat F>
inline auto
    CreateViewCubeArray(VkDevice device, VkImage image, uint32_t arrayLayers, VkImageAspectFlags aspect, uint32_t mips) -> std::expected<ImageView, Error> {
    return CreateView(device, MakeViewCreateInfoCubeArray(image, F, arrayLayers, aspect, mips));
}

template <VkFormat F>
inline auto CreateViewSingleMip(VkDevice device, VkImage image, uint32_t baseMip, VkImageAspectFlags aspect) -> std::expected<ImageView, Error> {
    return CreateView(device, MakeViewCreateInfo2D(image, F, 1, aspect, baseMip));
}

} // namespace ZHLN::Vk
