// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../FormatTag.hpp"
#include "../core/Handles.hpp"
#include <array>
#include <concepts>
#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN::Vk {

struct RuntimeAttachmentFormats {};

// The attachment set of a typed pass: the depth tag and the colour tags, in bind
// order. The VkFormat values the driver wants are derived from the tags below,
// so a tag is the identity of a slot and the array is a projection of it -- one
// source of truth, two forms, which is what the whole format-tag change is for.
//
// Format::Undefined is the "slot exists, format not statically known" marker. It
// is a real VkFormat value (VK_FORMAT_UNDEFINED), so it projects like any other
// tag; the difference from RuntimeAttachmentFormats is that the set still knows
// how many slots there are and which of them are depth.
template <Format::IsFormat DepthTag, Format::IsFormat... ColorTags>
struct AttachmentFormats {
    static constexpr VkFormat depth_format = DepthTag::vk;
    static constexpr std::array<VkFormat, sizeof...(ColorTags)> color_formats {ColorTags::vk...};

    // The same set in the other form. A caller that already has a pipeline and
    // needs its format as a *type* -- the variant loop that asks which of its
    // pipelines matches the swapchain's format -- reads the tag here instead of
    // projecting a value back into a template argument.
    using depth_tag = DepthTag;

    // Lazily evaluated, so a depth-only set (ColorTags... empty) is fine as long as
    // nothing asks for a colour tag. The constraint turns an accidental ask into a
    // rejected template argument rather than an error inside tuple_element_t.
    template <size_t I>
        requires(I < sizeof...(ColorTags))
    using color_tag = std::tuple_element_t<I, std::tuple<ColorTags...>>;
};

template <typename Formats, Format::IsFormat... Added>
struct AppendAttachmentColors {
    using type = RuntimeAttachmentFormats;
};

template <Format::IsFormat DepthTag, Format::IsFormat... ColorTags, Format::IsFormat... Added>
struct AppendAttachmentColors<AttachmentFormats<DepthTag, ColorTags...>, Added...> {
    using type = std::conditional_t<
        ((Added::vk != VK_FORMAT_UNDEFINED) && ...), AttachmentFormats<DepthTag, ColorTags..., Added...>, RuntimeAttachmentFormats
    >;
};

template <typename Formats, Format::IsFormat DepthTag>
struct SetAttachmentDepth {
    using type = RuntimeAttachmentFormats;
};

template <Format::IsFormat OldDepth, Format::IsFormat... ColorTags, Format::IsFormat DepthTag>
struct SetAttachmentDepth<AttachmentFormats<OldDepth, ColorTags...>, DepthTag> {
    using type = std::conditional_t<
        DepthTag::vk == VK_FORMAT_UNDEFINED, RuntimeAttachmentFormats, AttachmentFormats<DepthTag, ColorTags...>
    >;
};

template <typename Formats>
struct WithoutAttachmentDepth {
    using type = RuntimeAttachmentFormats;
};

template <Format::IsFormat DepthTag, Format::IsFormat... ColorTags>
struct WithoutAttachmentDepth<AttachmentFormats<DepthTag, ColorTags...>> {
    using type = AttachmentFormats<Format::Undefined, ColorTags...>;
};

template <typename Formats>
struct ClearAttachmentColors {
    using type = RuntimeAttachmentFormats;
};

template <Format::IsFormat DepthTag, Format::IsFormat... ColorTags>
struct ClearAttachmentColors<AttachmentFormats<DepthTag, ColorTags...>> {
    using type = AttachmentFormats<DepthTag>;
};

template <size_t ColorCount, bool HasDepth, typename Formats>
class PipelineBuilder;

template <size_t ColorCount, bool HasDepth, typename Formats = RuntimeAttachmentFormats>
class TypedPipeline {
  public:
    using FormatSet = Formats;

    TypedPipeline() = default;
    explicit TypedPipeline(Pipeline&& pipeline) noexcept requires std::same_as<Formats, RuntimeAttachmentFormats>:
        handle(std::move(pipeline)) {}

    auto operator=(Pipeline&& pipeline) noexcept -> TypedPipeline& requires std::same_as<Formats, RuntimeAttachmentFormats> {
        handle = std::move(pipeline);
        return *this;
    }

    [[nodiscard]] auto Get() const noexcept -> VkPipeline {
        return handle.Get();
    }
    [[nodiscard]] auto Valid() const noexcept -> bool {
        return handle.Valid();
    }
    explicit operator bool() const noexcept {
        return Valid();
    }
    [[nodiscard]] auto Release() noexcept -> Pipeline {
        return std::move(handle);
    }

  private:
    template <size_t, bool, typename>
    friend class PipelineBuilder;
    struct BuilderToken {};
    explicit TypedPipeline(Pipeline&& pipeline, BuilderToken) noexcept: handle(std::move(pipeline)) {}

    Pipeline handle;
};

} // namespace ZHLN::Vk
