// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vkd3d/Enums.hpp>

#include <vkd3d/VulkanConversions.hpp>

// Compile-time spot checks for the neutral<->Vulkan casts: a renamed
// enumerator or a drifted Vk:: value fails here instead of at a barrier.
// The D3D12 direction is checked in src/vkd3d/D3D12Conversions.cpp, the
// only TU allowed to name the real SDK constants.
static_assert(ZHLN::VkD3D::ToVk(ZHLN::VkD3D::ImageLayout::ColorAttachmentOptimal) == ZHLN::Vk::ImageLayout::ColorAttachmentOptimal);
static_assert(ZHLN::VkD3D::FromVk(ZHLN::Vk::Format::R8G8B8A8Srgb) == ZHLN::VkD3D::Format::R8G8B8A8Srgb);
static_assert(ZHLN::Vk::ToVk(ZHLN::VkD3D::ToVk(ZHLN::VkD3D::PipelineStage::Compute)) == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

// ColorMask bit correspondence with the raw Vulkan write mask.
static_assert(ZHLN::VkD3D::ToVk(ZHLN::VkD3D::ColorMask::Red) == VK_COLOR_COMPONENT_R_BIT);
static_assert(ZHLN::VkD3D::ToVk(ZHLN::VkD3D::ColorMask::Green) == VK_COLOR_COMPONENT_G_BIT);
static_assert(ZHLN::VkD3D::ToVk(ZHLN::VkD3D::ColorMask::Blue) == VK_COLOR_COMPONENT_B_BIT);
static_assert(ZHLN::VkD3D::ToVk(ZHLN::VkD3D::ColorMask::Alpha) == VK_COLOR_COMPONENT_A_BIT);
static_assert(ZHLN::VkD3D::ToVk(ZHLN::VkD3D::ColorMask::Red | ZHLN::VkD3D::ColorMask::Alpha) == (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_A_BIT));
static_assert(ZHLN::VkD3D::ToVk(ZHLN::VkD3D::ColorMask::None) == 0);
