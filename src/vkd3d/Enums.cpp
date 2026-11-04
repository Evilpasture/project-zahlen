// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Enums.hpp"

// Compile-time spot checks for the translation tables: a renamed enumerator
// or a transposed D3D12/DXGI value fails here instead of at a barrier.
static_assert(ZHLN::VkD3D::ToVk(ZHLN::VkD3D::ImageLayout::ColorAttachmentOptimal) == ZHLN::Vk::ImageLayout::ColorAttachmentOptimal);
static_assert(static_cast<uint32_t>(ZHLN::VkD3D::ToD3D12(ZHLN::VkD3D::ImageLayout::ColorAttachmentOptimal)) == 2); // D3D12_BARRIER_LAYOUT_RENDER_TARGET
static_assert(static_cast<uint32_t>(ZHLN::VkD3D::ToD3D12(ZHLN::VkD3D::Format::D32SfloatS8Uint)) == 20);           // DXGI_FORMAT_D32_FLOAT_S8X24_UINT
static_assert(ZHLN::VkD3D::FromVk(ZHLN::Vk::Format::R8G8B8A8Srgb) == ZHLN::VkD3D::Format::R8G8B8A8Srgb);
static_assert(ZHLN::Vk::ToVk(ZHLN::VkD3D::ToVk(ZHLN::VkD3D::PipelineStage::Compute)) == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
