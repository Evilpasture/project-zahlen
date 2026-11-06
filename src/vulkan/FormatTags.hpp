// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A render header, so it sits under the same rule as the rest of src/vulkan:
// Rendering.hpp comes first and supplies Vulkan (VulkanHeader.hpp -> <volk.h>)
// and the graph, hence TypeList. Naming the rule makes the requirement explicit
// instead of leaving it to whatever a translation unit happened to include
// before this, and it is what catches the mistake when someone includes this
// first. In particular: no <vulkan/vulkan.h> here. volk is this tree's single
// Vulkan entry point, and a second one is a conflict waiting for a TUs worth of
// distance.
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "FormatTag.hpp"          // Format::IsFormat, Format::Undefined
#include "graph/RenderPassDecl.hpp" // ZHLN::Vk::TypeList

#include <array>
#include <cstdint>
#include <type_traits>
#include <utility>

// Vulkan formats as types.
//
// A VkFormat is a value, so it can be *named* but not *walked*: Filter, partition
// and fold are all tools this graph already owns, and none of them apply to a
// std::integer_sequence<VkFormat, ...>. The tags here are the type-space form of
// the same information, so a format list becomes a TypeList and joins the
// machinery instead of sitting beside it.
//
// A tag is self-describing -- no FormatTraits<T> specialisation to forget. Every
// tag states its Vulkan format, the image aspect it is accessed through, its
// texel size, and whether it is depth-stencil, and SelfConsistentFormat checks
// the four agree.
//
// bytes_per_pixel is hand-tabulated because Vulkan exposes no API for it: there
// is no vkGetFormatTexelSize, only component/block metadata to reassemble. The
// values are transcribed from the format definitions in the spec, and the ones
// worth pausing on are recorded next to their tag.
namespace ZHLN::Vk::Format {

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------

struct B8G8R8A8_UNorm {
    static constexpr VkFormat           vk              = VK_FORMAT_B8G8R8A8_UNORM;
    static constexpr VkImageAspectFlags aspect          = VK_IMAGE_ASPECT_COLOR_BIT;
    static constexpr uint32_t           bytes_per_pixel = 4;
    static constexpr bool               is_depth_stencil = false;
};

struct B8G8R8A8_SRGB {
    static constexpr VkFormat           vk              = VK_FORMAT_B8G8R8A8_SRGB;
    static constexpr VkImageAspectFlags aspect          = VK_IMAGE_ASPECT_COLOR_BIT;
    static constexpr uint32_t           bytes_per_pixel = 4;
    static constexpr bool               is_depth_stencil = false;
};

struct R8G8B8A8_UNorm {
    static constexpr VkFormat           vk              = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkImageAspectFlags aspect          = VK_IMAGE_ASPECT_COLOR_BIT;
    static constexpr uint32_t           bytes_per_pixel = 4;
    static constexpr bool               is_depth_stencil = false;
};

struct R8G8B8A8_SRGB {
    static constexpr VkFormat           vk              = VK_FORMAT_R8G8B8A8_SRGB;
    static constexpr VkImageAspectFlags aspect          = VK_IMAGE_ASPECT_COLOR_BIT;
    static constexpr uint32_t           bytes_per_pixel = 4;
    static constexpr bool               is_depth_stencil = false;
};

struct R16G16B16A16_SFloat {
    static constexpr VkFormat           vk              = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkImageAspectFlags aspect          = VK_IMAGE_ASPECT_COLOR_BIT;
    static constexpr uint32_t           bytes_per_pixel = 8;
    static constexpr bool               is_depth_stencil = false;
};

struct R16G16_SFloat {
    static constexpr VkFormat           vk              = VK_FORMAT_R16G16_SFLOAT;
    static constexpr VkImageAspectFlags aspect          = VK_IMAGE_ASPECT_COLOR_BIT;
    static constexpr uint32_t           bytes_per_pixel = 4;
    static constexpr bool               is_depth_stencil = false;
};

struct B10G11R11_UFloatPack32 {
    static constexpr VkFormat           vk              = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    static constexpr VkImageAspectFlags aspect          = VK_IMAGE_ASPECT_COLOR_BIT;
    static constexpr uint32_t           bytes_per_pixel = 4;
    static constexpr bool               is_depth_stencil = false;
};

struct R8_UInt {
    static constexpr VkFormat           vk              = VK_FORMAT_R8_UINT;
    static constexpr VkImageAspectFlags aspect          = VK_IMAGE_ASPECT_COLOR_BIT;
    static constexpr uint32_t           bytes_per_pixel = 1;
    static constexpr bool               is_depth_stencil = false;
};

struct R8_UNorm {
    static constexpr VkFormat           vk              = VK_FORMAT_R8_UNORM;
    static constexpr VkImageAspectFlags aspect          = VK_IMAGE_ASPECT_COLOR_BIT;
    static constexpr uint32_t           bytes_per_pixel = 1;
    static constexpr bool               is_depth_stencil = false;
};

struct R8G8_UNorm {
    static constexpr VkFormat           vk              = VK_FORMAT_R8G8_UNORM;
    static constexpr VkImageAspectFlags aspect          = VK_IMAGE_ASPECT_COLOR_BIT;
    static constexpr uint32_t           bytes_per_pixel = 2;
    static constexpr bool               is_depth_stencil = false;
};

struct R32_SFloat {
    static constexpr VkFormat           vk              = VK_FORMAT_R32_SFLOAT;
    static constexpr VkImageAspectFlags aspect          = VK_IMAGE_ASPECT_COLOR_BIT;
    static constexpr uint32_t           bytes_per_pixel = 4;
    static constexpr bool               is_depth_stencil = false;
};

struct R32G32B32A32_SFloat {
    static constexpr VkFormat           vk              = VK_FORMAT_R32G32B32A32_SFLOAT;
    static constexpr VkImageAspectFlags aspect          = VK_IMAGE_ASPECT_COLOR_BIT;
    static constexpr uint32_t           bytes_per_pixel = 16;
    static constexpr bool               is_depth_stencil = false;
};

// ---------------------------------------------------------------------------
// Depth / depth-stencil
// ---------------------------------------------------------------------------

struct D32_SFloat {
    static constexpr VkFormat           vk              = VK_FORMAT_D32_SFLOAT;
    static constexpr VkImageAspectFlags aspect          = VK_IMAGE_ASPECT_DEPTH_BIT;
    static constexpr uint32_t           bytes_per_pixel = 4;
    static constexpr bool               is_depth_stencil = true;
};

// 64-bit texel, not 5 bytes: 32-bit depth in the low word and the 8-bit stencil
// in the low byte of the high word. Reading the components as 4 + 1 understates
// the stride by 3 bytes, which offsets every row after the first without any
// validation error to show for it.
struct D32_SFloat_S8_UInt {
    static constexpr VkFormat           vk              = VK_FORMAT_D32_SFLOAT_S8_UINT;
    static constexpr VkImageAspectFlags aspect          = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    static constexpr uint32_t           bytes_per_pixel = 8;
    static constexpr bool               is_depth_stencil = true;
};

// ---------------------------------------------------------------------------
// Everything a runtime format can be visited as
// ---------------------------------------------------------------------------

using AllFormats = TypeList<
    Undefined, B8G8R8A8_UNorm, B8G8R8A8_SRGB, R8G8B8A8_UNorm, R8G8B8A8_SRGB, R16G16B16A16_SFloat, R16G16_SFloat,
    B10G11R11_UFloatPack32, R8_UInt, R8_UNorm, R8G8_UNorm, R32_SFloat, R32G32B32A32_SFloat, D32_SFloat, D32_SFloat_S8_UInt>;

// Swapchain and on-disk formats are only known at runtime, so a tag has to be
// recovered from a VkFormat value somewhere. This is that bridge, and it is
// deliberately easy to notice when it fails: the result is [[nodiscard]], and an
// unhandled format returns false rather than quietly doing nothing. A visitor
// that silently no-ops on an unknown format shows up later as a black pass with
// no error to point at, which is a far worse trade than an ignored return value.
template <typename Fn>
[[nodiscard]] constexpr auto Visit(VkFormat format, Fn&& fn) noexcept -> bool {
    // Explicit -> void on both lambdas, deliberately. A generic lambda with a
    // deduced return type makes the return a placeholder until deduction, and
    // clang's Itanium mangler is asked for the signature at
    // ActOnStartOfLambdaDefinition -- before deduction -- for lambda numbering.
    // The p2996 fork asserts ("mangling a placeholder type") on exactly that.
    // Nothing here returns a value, so naming void costs nothing and keeps this
    // construct out of that path.
    bool handled = false;
    [&]<typename... Fs>(TypeList<Fs...>) -> void {
        (([&]() -> void {
             if (!handled && Fs::vk == format) {
                 handled = true;
                 fn.template operator()<Fs>();
             }
         }()),
         ...);
    }(AllFormats {});
    return handled;
}

// The value-space form of the same list, for the places that still take a pack of
// VkFormat (AttachmentFormats and the pipeline types are value-templated today).
// Deriving it keeps one source of truth: the TypeList stays authoritative and the
// integer_sequence is a projection of it rather than a parallel declaration.
template <typename List>
struct AsVkFormats;

template <typename... Fs>
struct AsVkFormats<TypeList<Fs...>> {
    static_assert((IsFormat<Fs> && ...), "AsVkFormats holds format tags only");
    using type = std::integer_sequence<VkFormat, Fs::vk...>;
};

template <typename List>
using AsVkFormatsT = typename AsVkFormats<List>::type;

// ---------------------------------------------------------------------------
// Compile-time proof that the table above is internally consistent
// ---------------------------------------------------------------------------

namespace TemplatedDetail {

template <typename... Fs>
consteval auto AllSelfConsistent(TypeList<Fs...>) -> bool {
    return (SelfConsistentFormat<Fs> && ...);
}

template <typename... Fs>
consteval auto AllDistinct(TypeList<Fs...>) -> bool {
    constexpr std::array<VkFormat, sizeof...(Fs)> all {Fs::vk...};
    for (size_t i = 0; i < all.size(); ++i) {
        for (size_t j = i + 1; j < all.size(); ++j) {
            if (all[i] == all[j]) {
                return false;
            }
        }
    }
    return true;
}

} // namespace TemplatedDetail

static_assert(TemplatedDetail::AllSelfConsistent(AllFormats {}), "every tag's aspect must agree with its is_depth_stencil flag");
static_assert(TemplatedDetail::AllDistinct(AllFormats {}), "two tags must not name the same VkFormat: Visit would resolve the first only");

static_assert(ColorFormat<B8G8R8A8_SRGB> && ColorFormat<R8_UInt>);
static_assert(!ColorFormat<D32_SFloat> && DepthStencilFormat<D32_SFloat_S8_UInt>);
static_assert(!ColorFormat<Undefined> && !DepthStencilFormat<Undefined>);
static_assert(D32_SFloat_S8_UInt::bytes_per_pixel == 8, "D32_SFLOAT_S8_UINT is a 64-bit texel");
static_assert(D32_SFloat::bytes_per_pixel == 4);
static_assert(R16G16B16A16_SFloat::bytes_per_pixel == 8);

} // namespace ZHLN::Vk::Format
