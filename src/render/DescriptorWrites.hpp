// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/DescriptorWrites.hpp
//
// Argument types consumed by the VK_EXT_descriptor_heap write helpers
// (HeapBindings.hpp). These are the survivors of the old descriptor-set DSL:
// the engine passes descriptor updates to HeapManager::WriteBindings in
// declaration order, and the helper translates each argument into a
// vkWriteResourceDescriptorsEXT / vkWriteSamplerDescriptorsEXT write.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

struct ImageWrite {
    VkImageView   view   = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // Parameters used to create `view`: heap image descriptors consume
    // VkImageViewCreateInfo instead of handles. Optional when the writer can
    // synthesize a default 2D view info.
    const VkImageViewCreateInfo* viewInfo = nullptr;
};

struct BufferWrite {
    VkBuffer     buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize range  = VK_WHOLE_SIZE;
};

/// Sentinel: skips a binding slot (e.g. a dead-code-eliminated trailing sampler).
struct SkipWrite {};

/// True for compile-time-layout-tracked images (TypedImage<L>), whose layout is
/// part of the type.
template <typename T>
struct IsTypedImage: std::false_type {};
template <VkImageLayout L>
struct IsTypedImage<TypedImage<L>>: std::true_type {};

} // namespace ZHLN::Vk
