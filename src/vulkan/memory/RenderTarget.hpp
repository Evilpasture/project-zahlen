// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Defer.hpp>

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

// Rendering.hpp includes this header before RenderGraph.hpp. The pure graph
// tag lives in the joint layer (<vkd3d/graph/GraphImage.hpp>) so physical
// targets can depend on it without making compile-time graph analysis depend
// on GPU allocation code. The tag vocabulary is neutral; this layer keeps the
// Vk:: spellings and converts at this boundary.
#include <vkd3d/VulkanConversions.hpp>
#include <vkd3d/graph/GraphImage.hpp>

namespace ZHLN::Vk {

// A physical image can be described by a format alone. Graph-specific
// allocation policy is opt-in through the tag, never required for raw targets.
template <typename T>
struct TargetTraits {
    static constexpr Vk::Format           format = VkD3D::ToVk(T::format);
    static constexpr Vk::ImageAspect aspect = [] {
        if constexpr (requires { T::aspect; })
            return VkD3D::ToVk(T::aspect);
        else
            return GetFormatAspect(format);
    }();
    static constexpr std::string_view name = [] {
        if constexpr (requires { T::name.string_view(); })
            return T::name.string_view();
        else if constexpr (requires { T::name; })
            return std::string_view(T::name);
        else
            return std::string_view {};
    }();
    static constexpr uint32_t scale_divisor = [] {
        if constexpr (requires { T::scale_divisor; })
            return T::scale_divisor;
        else
            return 1u;
    }();
    static constexpr bool is_3d = [] {
        if constexpr (requires { T::is_3d; })
            return T::is_3d;
        else
            return false;
    }();
    static constexpr bool is_mipmapped = [] {
        if constexpr (requires { T::is_mipmapped; })
            return T::is_mipmapped;
        else
            return false;
    }();
    static constexpr bool is_persistent = [] {
        if constexpr (requires { T::is_persistent; })
            return T::is_persistent;
        else
            return false;
    }();
    static constexpr bool is_swapchain = [] {
        if constexpr (requires { T::is_swapchain; })
            return T::is_swapchain;
        else
            return false;
    }();
};

template <typename Tag>
struct RenderTarget {
    using TagType                                     = Tag;
    using Traits                                      = TargetTraits<Tag>;
    static constexpr std::string_view   name          = Traits::name;
    static constexpr Vk::Format           format        = Traits::format;
    static constexpr Vk::ImageAspect aspect        = Traits::aspect;
    static constexpr bool               is_swapchain  = Traits::is_swapchain;
    static constexpr bool               is_persistent = Traits::is_persistent;
    static constexpr uint32_t           scale_divisor = Traits::scale_divisor;
    static constexpr bool               is_3d         = Traits::is_3d;
    static constexpr bool               is_mipmapped  = Traits::is_mipmapped;

    static_assert(scale_divisor > 0, "Render target scale divisor must be nonzero");
    static_assert(!is_3d || (!is_mipmapped && scale_divisor == 1), "3D targets do not use 2D mip/scaling policy");
    static_assert(!is_mipmapped || Has(aspect, Vk::ImageAspect::Color), "Mipmapped graph targets must be color images");

    // Keep extent's dimensional type: many callers pass a 2D extent directly
    // to DynamicPass, while voxel targets must retain their depth dimension.
    using Extent = std::conditional_t<is_3d, VkExtent3D, VkExtent2D>;
    Image                       image;
    AttachmentView              attachmentView;
    std::vector<AttachmentView> mipAttachmentViews;
    Extent                      extent {};
    uint32_t                    mipLevels = 1;

    RenderTarget()                                           = default;
    RenderTarget(const RenderTarget&)                        = delete;
    auto operator=(const RenderTarget&) -> RenderTarget&     = delete;
    RenderTarget(RenderTarget&&) noexcept                    = default;
    auto operator=(RenderTarget&&) noexcept -> RenderTarget& = default;
    ~RenderTarget()                                          = default;

    [[nodiscard]] auto View() const noexcept -> ImageView {
        return image.View().WithAspect(aspect);
    }
    [[nodiscard]] auto State() const noexcept -> TypedImage<Vk::ImageLayout::Undefined> {
        return AsSlice().template Assume<Vk::ImageLayout::Undefined>();
    }
    [[nodiscard]] auto AsSlice() const noexcept -> ImageSlice {
        return ImageSlice {image.Handle(), attachmentView, extent, format, aspect};
    }

    template <Vk::ImageLayout OldLayout, Vk::ImageLayout NewLayout>
    void Transition(VkCommandBuffer cmd) const noexcept {
        View().template Transition<OldLayout, NewLayout>(cmd);
    }

    template <Vk::ImageLayout TargetLayout>
    void DiscardTo(VkCommandBuffer cmd) const noexcept {
        View().template DiscardTo<TargetLayout>(cmd);
    }
    [[nodiscard]] auto Valid() const noexcept -> bool {
        return image.Valid() && attachmentView.Valid();
    }
    explicit operator bool() const noexcept {
        return Valid();
    }

    void Destroy(Allocator& allocator) noexcept {
        mipAttachmentViews.clear();
        attachmentView = {};
        allocator.DestroyImage(image);
        extent    = {};
        mipLevels = 1;
    }

    // Explicit descriptor for physical allocations with non-DAG usage or
    // array-layer policies (e.g. headless color and shadow cascades).
    struct RenderTargetDescriptor {
        ImageUsage usage          = ImageUsage::None;
        uint32_t   arrayLayers    = 1;
        uint32_t   mipLevels      = 1;
        bool       cubeCompatible = false;
    };

    [[nodiscard]] static auto
        Create(Allocator& allocator, VkDevice device, VkExtent2D extent2D, RenderTargetDescriptor desc) -> std::expected<RenderTarget, ErrorCode>
        requires(!is_3d)
    {
        RenderTarget rt;
        rt.extent                  = extent2D;
        rt.mipLevels               = std::max(1u, desc.mipLevels);
        auto imageConfig           = ImageConfig::Texture2D(extent2D, format, desc.usage, rt.mipLevels, desc.arrayLayers);
        imageConfig.cubeCompatible = desc.cubeCompatible;
        return CreateFromConfig(allocator, device, std::move(rt), imageConfig);
    }

    // Graph-owned target. Extra usage is strictly additive; format-specific
    // features and exceptional fixed resolutions are chosen by TargetManager.
    [[nodiscard]] static auto
        Create(Allocator& allocator, VkDevice device, VkExtent2D fullExtent, VkExtent3D voxelExtent, ImageUsage extraUsage = ImageUsage::None)
            -> std::expected<RenderTarget, ErrorCode> {
        RenderTarget rt;
        if constexpr (is_3d) {
            rt.extent   = voxelExtent;
            auto config = ImageConfig::Texture3D(voxelExtent, format, ImageUsage::Storage | ImageUsage::Sampled | ImageUsage::TransferDst | extraUsage);
            return CreateFromConfig(allocator, device, std::move(rt), config);
        } else {
            rt.extent        = {std::max(1u, fullExtent.width / scale_divisor), std::max(1u, fullExtent.height / scale_divisor)};
            ImageUsage usage = ImageUsage::Sampled | extraUsage;
            if constexpr (Has(aspect, Vk::ImageAspect::Depth))
                usage |= ImageUsage::DepthStencilAttachment;
            else
                usage |= ImageUsage::ColorAttachment;
            if constexpr (scale_divisor > 1)
                usage |= ImageUsage::Storage;
            if constexpr (is_mipmapped) {
                rt.mipLevels = GetMipLevels(rt.extent.width, rt.extent.height);
                usage |= ImageUsage::TransferSrc | ImageUsage::TransferDst;
            }
            auto config = ImageConfig::Texture2D(rt.extent, format, usage, rt.mipLevels);
            return CreateFromConfig(allocator, device, std::move(rt), config);
        }
    }

  private:
    [[nodiscard]] static auto
        CreateFromConfig(Allocator& allocator, VkDevice device, RenderTarget rt, ImageConfig config) -> std::expected<RenderTarget, ErrorCode> {
        auto img = Image::Create(allocator, config);
        if (!img)
            return std::unexpected(img.error());
        rt.image = std::move(*img);
        ZHLN::defer cleanup([&] { rt.Destroy(allocator); });
        // The view preserves depth/stencil aspects and all image mip levels,
        // just as the three former target implementations did.
        auto view = rt.image.CreateAttachmentView(device, aspect);
        if (!view)
            return std::unexpected(view.error());
        rt.attachmentView = std::move(*view);
        if constexpr (is_mipmapped) {
            rt.mipAttachmentViews.reserve(rt.mipLevels);
            for (uint32_t m = 0; m < rt.mipLevels; ++m) {
                auto mipView = rt.image.CreateAttachmentView(device, aspect, m, 1);
                if (!mipView)
                    return std::unexpected(mipView.error());
                rt.mipAttachmentViews.push_back(std::move(*mipView));
            }
        }
        return std::move(rt);
    }
};

// An untagged physical allocation still has a format and inferred aspect.
template <Vk::Format F, Vk::ImageAspect Aspect = GetFormatAspect(F)>
struct ImageFormat {
    static constexpr Vk::Format           format = F;
    static constexpr Vk::ImageAspect aspect = Aspect;
};

template <Vk::Format F>
using FormatRenderTarget = RenderTarget<ImageFormat<F>>;

template <Vk::ImageLayout TargetLayout, typename GraphTag>
[[nodiscard]] constexpr auto Transition(VkCommandBuffer cmd, const RenderTarget<GraphTag>& rt, Tag<TargetLayout>) noexcept;

template <Vk::ImageLayout L, typename Tag>
Vk::TypedImage<L> AssumeLayout(const Vk::RenderTarget<Tag>& rt, Vk::ImageAspect aspect = RenderTarget<Tag>::aspect) {
    return rt.AsSlice().template Assume<L>(aspect);
}

template <typename Usage>
    requires requires { typename Usage::Resource; }
struct UsageLayout {
    static_assert(requires { typename Usage::Resource; }, "UsageLayout requires a valid VkD3D::Usage type.");

    static constexpr Vk::ImageLayout      layout = VkD3D::ToVk(Usage::layout);
    static constexpr Vk::ImageAspect aspect = VkD3D::ToVk(Usage::Resource::aspect);
};

template <typename Usage, typename T>
[[nodiscard]] constexpr auto Assume(const T& resource) noexcept {
    using Layout = UsageLayout<Usage>;
    return AssumeLayout<Layout::layout>(resource, Layout::aspect);
}

template <Vk::ImageLayout TargetLayout, typename T>
constexpr auto TransitionSingle(VkCommandBuffer cmd, const T& res) noexcept {
    return std::get<0>(TransitionBatch<TargetLayout>(cmd, res));
}

template <Vk::ImageLayout TargetLayout, typename... Resources>
[[nodiscard]] constexpr auto TransitionBatch(VkCommandBuffer cmd, const Resources&... resources) noexcept;

template <Vk::ImageLayout L, typename Tuple>
[[nodiscard]] auto TransitionAllTo(VkCommandBuffer cmd, const Tuple& atts) {
    return std::apply([&](const auto&... a) { return Vk::TransitionBatch<L>(cmd, a...); }, atts);
}

template <typename... Targets>
struct RenderTargetBundle {
    std::tuple<Targets&...> targets;

    constexpr explicit RenderTargetBundle(Targets&... t) noexcept: targets(t...) {
    }

    [[nodiscard]] auto Recreate(Allocator& alloc, VkDevice device, VkExtent2D extent) -> std::expected<void, ErrorCode> {
        std::expected<void, ErrorCode> result;
        std::apply(
            [&](auto&... t) {
                (
                    [&] {
                        if (!result)
                            return;
                        auto created = std::remove_cvref_t<decltype(t)>::Create(
                            alloc, device, extent, typename std::remove_cvref_t<decltype(t)>::RenderTargetDescriptor {}
                        );
                        if (!created) {
                            result = std::unexpected(created.error());
                            return;
                        }
                        t.Destroy(alloc);
                        t = std::move(*created);
                    }(),
                    ...);
            },
            targets
        );
        return result;
    }

    template <Vk::ImageLayout TargetLayout>
    [[nodiscard]] constexpr auto Transition(VkCommandBuffer cmd) const noexcept {
        return std::apply([&](const auto&... t) { return TransitionBatch<TargetLayout>(cmd, t...); }, targets);
    }
};

template <typename... Ts>
[[nodiscard]] constexpr auto TieTargets(Ts&... tgts) noexcept {
    return RenderTargetBundle<Ts...>(tgts...);
}

template <typename... Images>
[[nodiscard]] auto ClearAndPrepareGroup(VkCommandBuffer cmd, VkExtent2D extent, Color4 clear, Images&... imgs) {
    auto bundle = Vk::TieTargets(imgs...);
    auto atts   = bundle.template Transition<Vk::ImageLayout::ColorAttachmentOptimal>(cmd);
    Vk::DynamicPass(extent).AddColorGroup(atts, ColorOp::Clear(clear)).Execute(cmd, []() {});
    return TransitionAllTo<Vk::ImageLayout::ShaderReadOnlyOptimal>(cmd, atts);
}

} // namespace ZHLN::Vk

#include "RenderTarget.inl"
