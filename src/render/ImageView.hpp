#pragma once

namespace ZHLN::Vk {

// ============================================================================
// Image View Helpers
// ============================================================================

[[nodiscard]] constexpr auto GetFormatAspect(VkFormat format) noexcept -> VkImageAspectFlags;

template <VkFormat F>
[[nodiscard]] auto CreateView(VkDevice device, VkImage image, VkImageAspectFlags aspect = GetFormatAspect(F), uint32_t mips = 1) -> ImageView;

template <VkFormat F>
[[nodiscard]] auto CreateViewCube(VkDevice device, VkImage image, uint32_t mips = 1) -> ImageView;

template <VkFormat F>
[[nodiscard]] auto CreateView2DArray(
    VkDevice           device,
    VkImage            image,
    uint32_t           baseLayer,
    uint32_t           layerCount,
    VkImageAspectFlags aspect = GetFormatAspect(F),
    uint32_t           mips   = 1
) -> ImageView;

template <VkFormat F>
[[nodiscard]] auto
    CreateViewCubeArray(VkDevice device, VkImage image, uint32_t arrayLayers, VkImageAspectFlags aspect = GetFormatAspect(F), uint32_t mips = 1) -> ImageView;
} // namespace ZHLN::Vk
#include "ImageView.inl"
