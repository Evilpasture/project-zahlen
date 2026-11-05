// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../memory/ImageView.hpp"
#include "DynamicRendering.hpp"
#include <vkd3d/FormatAspect.hpp>
#include <vkd3d/RenderPassDecl.hpp>
#include <array>
#include <string_view>

namespace ZHLN::Vk {

// ResourceName is graph vocabulary, not Vulkan: it moved to the joint layer
// (src/vkd3d/include/vkd3d/RenderPassDecl.hpp) and is re-exported here so
// existing Vk::ResourceName spellings keep working.
using ZHLN::VkD3D::ResourceName;


// A graph resource's identity and allocation policy. This is a description, not
// a Vulkan object: the format it names is the neutral VkD3D::Format, so the
// tag -- and everything that compares or switches on it -- is backend-neutral.
// The Vulkan image that eventually backs it is created from ToVk(tag::format)
// at the one place that calls vkCreateImage.
template <
    ResourceName           Name,
    VkD3D::Format          Format,
    VkD3D::ImageAspect     Aspect       = VkD3D::GetFormatAspect(Format),
    bool                   IsSwapchain  = false,
    bool                   IsPersistent = false,
    uint32_t               ScaleDivisor = 1,
    bool                   Is3D         = false,
    bool                   IsMipmapped  = false>
struct GraphImageTag {
    static constexpr auto                       name          = Name;
    static constexpr VkD3D::Format              format        = Format;
    static constexpr VkD3D::ImageAspect         aspect        = Aspect;
    static constexpr bool                       is_swapchain  = IsSwapchain;
    static constexpr bool                       is_persistent = IsPersistent;
    static constexpr uint32_t                   scale_divisor = ScaleDivisor;
    static constexpr bool                       is_3d         = Is3D;
    static constexpr bool                       is_mipmapped  = IsMipmapped;
};

// Compatibility spelling: Vk::GraphImage still takes Vk::Format and
// Vk::ImageAspect at the call site, converts once, and hands the neutral tag
// to the graph. GraphImage<...> IS GraphImageTag<...>; there is one type, so
// resource identity, ClearColorOf specialization and every static_assert that
// names it keep working across the two spellings.
template <
    ResourceName           Name,
    Vk::Format             Format,
    Vk::ImageAspect        Aspect       = GetFormatAspect(Format),
    bool                   IsSwapchain  = false,
    bool                   IsPersistent = false,
    uint32_t               ScaleDivisor = 1,
    bool                   Is3D         = false,
    bool                   IsMipmapped  = false>
using GraphImage = GraphImageTag<
    Name, VkD3D::FromVk(Format), VkD3D::FromVk(Aspect), IsSwapchain, IsPersistent, ScaleDivisor, Is3D, IsMipmapped>;

template <typename Tag>
struct ClearColorOf {
    static constexpr Color4 value = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
};

} // namespace ZHLN::Vk
