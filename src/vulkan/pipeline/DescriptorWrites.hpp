// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/DescriptorWrites.hpp
//
// Field types of the VK_EXT_descriptor_heap parameter blocks (see
// src/render/PassParameters.hpp) as consumed by the write helpers
// (HeapBindings.hpp). These are the survivors of the old descriptor-set DSL:
// the helper translates each field into a vkWriteResourceDescriptorsEXT /
// vkWriteSamplerDescriptorsEXT write, with the reflected descriptor type of the
// binding the field pairs with deciding which.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
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

/// Sentinel field: this binding's descriptor is written somewhere else (static
/// slots, once per frame pair, ...), so the field is present only to keep the
/// block's field sequence aligned with the shader's binding sequence.
struct SkipWrite {};

/// True for compile-time-layout-tracked images (TypedImage<L>), whose layout is
/// part of the type.
template <typename T>
struct IsTypedImage: std::false_type {};
template <VkImageLayout L>
struct IsTypedImage<TypedImage<L>>: std::true_type {};

} // namespace ZHLN::Vk
