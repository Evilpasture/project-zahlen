// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Defer.hpp>

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

template <VkFormat F>
struct RenderTarget {
    Image      image;
    ImageView  view;
    VkExtent2D extent {};

    RenderTarget() = default;

    RenderTarget(const RenderTarget&)                    = delete;
    auto operator=(const RenderTarget&) -> RenderTarget& = delete;

    RenderTarget(RenderTarget&& other) noexcept;
    auto operator=(RenderTarget&& other) noexcept -> RenderTarget&;

    ~RenderTarget() = default;

    [[nodiscard]] auto State() const noexcept -> TypedImage<VK_IMAGE_LAYOUT_UNDEFINED>;

    struct RenderTargetDescriptor {
        ImageUsage         usage       = ImageUsage::None;
        VkImageAspectFlags aspect      = GetFormatAspect(F);
        uint32_t           arrayLayers = 1;
        uint32_t           mipLevels   = 1;
    };

    [[nodiscard]] static auto
        Create(Allocator& allocator, const Context& ctx, VkExtent2D extent, RenderTargetDescriptor desc) -> std::expected<RenderTarget, ErrorCode>;

    void Destroy(Allocator& allocator) noexcept {
        view = {};
        allocator.DestroyImage(image);
        extent = {};
    }

    [[nodiscard]] auto AsSlice() const noexcept -> ImageSlice {
        return ImageSlice {image.Handle(), view, extent, F};
    }

    [[nodiscard]] auto Valid() const noexcept -> bool;
    explicit           operator bool() const noexcept;
};

template <VkFormat F>
struct RenderTarget3D {
    Image      image;
    ImageView  view;
    VkExtent3D extent {};

    RenderTarget3D()  = default;
    ~RenderTarget3D() = default;

    RenderTarget3D(const RenderTarget3D&)            = delete;
    RenderTarget3D& operator=(const RenderTarget3D&) = delete;
    RenderTarget3D(RenderTarget3D&&) noexcept        = default;
    auto operator=(RenderTarget3D&& other) noexcept -> RenderTarget3D& {
        if (this != &other) {
            view   = std::move(other.view);
            image  = std::move(other.image);
            extent = other.extent;
        }
        return *this;
    }

    void Destroy(Allocator& allocator) noexcept {
        view = {};
        allocator.DestroyImage(image);
        extent = {};
    }

    [[nodiscard]] auto AsSlice() const noexcept -> ImageSlice {
        return ImageSlice {image.Handle(), view, extent, F};
    }

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return image.Valid() && view.Valid();
    }
    explicit operator bool() const noexcept {
        return Valid();
    }

    [[nodiscard]] static auto Create(Allocator& allocator, const Context& ctx, VkExtent3D extent, ImageUsage usage) -> std::expected<RenderTarget3D, ErrorCode>;
};

template <VkFormat F>
struct MipmappedRenderTarget {
    Image                  image;
    ImageView              fullView;
    std::vector<ImageView> mipViews;
    VkExtent2D             extent {};
    uint32_t               mipLevels = 1;

    MipmappedRenderTarget() = default;

    MipmappedRenderTarget(const MipmappedRenderTarget&)                    = delete;
    auto operator=(const MipmappedRenderTarget&) -> MipmappedRenderTarget& = delete;

    MipmappedRenderTarget(MipmappedRenderTarget&& other) noexcept = default;
    auto operator=(MipmappedRenderTarget&& other) noexcept -> MipmappedRenderTarget& {
        if (this != &other) {
            mipViews  = std::move(other.mipViews);
            fullView  = std::move(other.fullView);
            image     = std::move(other.image);
            extent    = other.extent;
            mipLevels = other.mipLevels;
        }
        return *this;
    }

    ~MipmappedRenderTarget() = default;

    [[nodiscard]] static auto
        Create(Allocator& allocator, const Context& ctx, VkExtent2D extent, ImageUsage usage) -> std::expected<MipmappedRenderTarget, ErrorCode> {
        MipmappedRenderTarget target;
        target.extent    = extent;
        target.mipLevels = GetMipLevels(extent.width, extent.height);

        const VkImageCreateInfo info = {
            .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .imageType             = VK_IMAGE_TYPE_2D,
            .format                = F,
            .extent                = {.width = extent.width, .height = extent.height, .depth = 1},
            .mipLevels             = target.mipLevels,
            .arrayLayers           = 1,
            .samples               = VK_SAMPLE_COUNT_1_BIT,
            .tiling                = VK_IMAGE_TILING_OPTIMAL,
            .usage                 = ToVk(usage),
            .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices   = nullptr,
            .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        auto img_res = Image::Create(allocator.Get(), info, MemoryUsage::GPUOnly);
        if (!img_res.has_value()) {
            return std::unexpected(img_res.error());
        }
        target.image = std::move(img_res.value());
        ZHLN::defer _([&] { target.Destroy(allocator); });

        const VkImageAspectFlags aspect   = GetFormatAspect(F);
        auto                     view_res = ImageView::Create(ctx.Device(), MakeViewCreateInfo2D(target.image.Handle(), F, target.mipLevels, aspect));
        if (!view_res.has_value()) {
            return std::unexpected(view_res.error());
        }
        target.fullView = std::move(*view_res);
        target.mipViews.reserve(target.mipLevels);
        for (uint32_t m = 0; m < target.mipLevels; ++m) {
            auto mip_res = ImageView::Create(ctx.Device(), MakeViewCreateInfo2D(target.image.Handle(), F, 1, aspect, m));
            if (!mip_res.has_value()) {
                return std::unexpected(mip_res.error());
            }
            target.mipViews.push_back(std::move(*mip_res));
        }
        return std::move(target); // Move before the failure guard runs.
    }

    void Destroy(Allocator& allocator) noexcept {
        mipViews.clear();
        fullView = {};
        allocator.DestroyImage(image);
        extent = {};
        mipLevels = 1;
    }

    [[nodiscard]] auto AsSlice() const noexcept -> ImageSlice {
        return ImageSlice {image.Handle(), fullView, extent, F};
    }

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return image.Valid() && fullView.Valid();
    }
    explicit operator bool() const noexcept {
        return Valid();
    }
};

template <VkImageLayout TargetLayout, VkFormat F>
[[nodiscard]] constexpr auto Transition(VkCommandBuffer cmd, const RenderTarget<F>& rt, Tag<TargetLayout>) noexcept;

template <typename T>
struct TargetFormat;

template <VkFormat F>
struct TargetFormat<Vk::RenderTarget<F>> {
    static constexpr VkFormat value = F;
};

template <typename... Targets>
struct GBufferLayout {
    static constexpr size_t count = sizeof...(Targets);

    template <size_t Index>
    using TargetTypeAt = Targets...[Index];

    template <size_t Index>
        requires(Index < count)
    static constexpr VkFormat get() {
        static_assert(Index < count, "GBuffer layout index out of bounds.");
        return TargetFormat<TargetTypeAt<Index>>::value;
    }

    static constexpr std::array<VkFormat, count> array = {TargetFormat<Targets>::value...};
};

template <VkImageLayout L, VkFormat F>
Vk::TypedImage<L> AssumeLayout(const Vk::RenderTarget<F>& rt, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    return rt.AsSlice().template Assume<L>(aspect);
}

template <VkImageLayout L, VkFormat F>
Vk::TypedImage<L> AssumeLayout(const Vk::RenderTarget3D<F>& rt, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    return rt.AsSlice().template Assume<L>(aspect);
}

template <VkImageLayout L, VkFormat F>
Vk::TypedImage<L> AssumeLayout(const Vk::MipmappedRenderTarget<F>& rt, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    return rt.AsSlice().template Assume<L>(aspect);
}

template <typename Usage>
    requires requires { typename Usage::Resource; }
struct UsageLayout {
    static_assert(requires { typename Usage::Resource; }, "UsageLayout requires a valid Vk::Usage type.");

    static constexpr VkImageLayout      layout = Usage::layout;
    static constexpr VkImageAspectFlags aspect = Usage::Resource::aspect;
};

template <typename Usage, typename T>
[[nodiscard]] constexpr auto Assume(const T& resource) noexcept {
    using Layout = UsageLayout<Usage>;
    return AssumeLayout<Layout::layout>(resource, Layout::aspect);
}

template <VkImageLayout TargetLayout, typename T>
constexpr auto TransitionSingle(VkCommandBuffer cmd, const T& res) noexcept {
    return std::get<0>(TransitionBatch<TargetLayout>(cmd, res));
}

template <VkImageLayout TargetLayout, typename... Resources>
[[nodiscard]] constexpr auto TransitionBatch(VkCommandBuffer cmd, const Resources&... resources) noexcept;

template <VkImageLayout L, typename Tuple>
[[nodiscard]] auto TransitionAllTo(VkCommandBuffer cmd, const Tuple& atts) {
    return std::apply([&](const auto&... a) { return Vk::TransitionBatch<L>(cmd, a...); }, atts);
}

template <typename... Targets>
struct RenderTargetBundle {
    std::tuple<Targets&...> targets;

    constexpr explicit RenderTargetBundle(Targets&... t) noexcept: targets(t...) {
    }

    [[nodiscard]] auto Recreate(Allocator& alloc, const Context& ctx, VkExtent2D extent) -> std::expected<void, ErrorCode> {
        std::expected<void, ErrorCode> result;
        std::apply([&](auto&... t) {
            ([&] {
                if (!result) return;
                auto created = std::remove_cvref_t<decltype(t)>::Create(alloc, ctx, extent, {});
                if (!created) {
                    result = std::unexpected(created.error());
                    return;
                }
                t.Destroy(alloc); // Caller must have waited for GPU use.
                t = std::move(*created);
            }(), ...);
        }, targets);
        return result;
    }

    template <VkImageLayout TargetLayout>
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
    auto atts   = bundle.template Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd);
    Vk::DynamicPass(extent).AddColorGroup(atts, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, clear).Execute(cmd, []() {});
    return TransitionAllTo<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, atts);
}

} // namespace ZHLN::Vk

#include "RenderTarget.inl"
