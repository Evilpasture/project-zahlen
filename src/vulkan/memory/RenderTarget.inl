#pragma once

#include "RenderTarget.hpp"

namespace ZHLN::Vk {

// RenderTarget Implementation

template <VkFormat F>
inline RenderTarget<F>::RenderTarget(RenderTarget&& other) noexcept:
    image(std::move(other.image)), view(std::move(other.view)), extent(other.extent) {
}

template <VkFormat F>
inline auto RenderTarget<F>::operator=(RenderTarget&& other) noexcept -> RenderTarget& {
    if (this != &other) {
        view   = std::move(other.view);
        image  = std::move(other.image);
        extent = other.extent;
    }
    return *this;
}

template <VkFormat F>
inline auto RenderTarget<F>::State() const noexcept -> TypedImage<VK_IMAGE_LAYOUT_UNDEFINED> {
    return AsSlice().template Assume<VK_IMAGE_LAYOUT_UNDEFINED>();
}

template <VkFormat F>
inline auto
    RenderTarget<F>::Create(Allocator& allocator, const Context& ctx, VkExtent2D extent, RenderTargetDescriptor desc) -> std::expected<RenderTarget, Vk::Error> {
    RenderTarget rt;
    rt.extent = extent;

    const uint32_t mips = desc.mipLevels ? desc.mipLevels : 1;
    ImageConfig imageConfig = ImageConfig::Texture2D(extent, F, desc.usage, mips, desc.arrayLayers);
    imageConfig.cubeCompatible = desc.cubeCompatible;

    auto img_res = Image::Create(allocator, imageConfig);
    if (!img_res.has_value()) {
        return std::unexpected(img_res.error());
    }
    rt.image = std::move(img_res.value());
    ZHLN::defer _([&] { rt.Destroy(allocator); });

    const ImageViewConfig viewConfig {
        .kind = desc.arrayLayers > 1 ? ImageViewKind::Texture2DArray : ImageViewKind::Texture2D,
    };
    auto view_res = rt.image.CreateView(ctx.Device(), viewConfig);
    if (!view_res.has_value()) {
        return std::unexpected(view_res.error());
    }
    rt.view = std::move(*view_res);
    return std::move(rt); // Move before the failure guard runs (NRVO would destroy the result).
}

template <VkFormat F>
inline auto RenderTarget<F>::Valid() const noexcept -> bool {
    return image.Valid() && view.Valid();
}

template <VkFormat F>
inline RenderTarget<F>::operator bool() const noexcept {
    return Valid();
}

template <VkFormat F>
inline auto
    RenderTarget3D<F>::Create(Allocator& allocator, const Context& ctx, VkExtent3D extent, ImageUsage usage) -> std::expected<RenderTarget3D, Vk::Error> {
    RenderTarget3D rt;
    rt.extent                    = extent;
    const ImageConfig imageConfig = ImageConfig::Texture3D(extent, F, usage);
    auto img_res = Image::Create(allocator, imageConfig);
    if (!img_res.has_value()) {
        return std::unexpected(img_res.error());
    }
    rt.image = std::move(img_res.value());
    ZHLN::defer _([&] { rt.Destroy(allocator); });

    auto view_res = rt.image.CreateView(ctx.Device(), {.kind = ImageViewKind::Texture3D});
    if (!view_res.has_value()) {
        return std::unexpected(view_res.error());
    }
    rt.view = std::move(*view_res);
    return std::move(rt); // Move before the failure guard runs (NRVO would destroy the result).
}

// Transition Helpers

namespace TemplatedDetail {

template <typename T>
struct ResourceTraits;

template <VkImageLayout Layout, VkFormat Format>
struct ResourceTraits<TypedImage<Layout, Format>> {
    static constexpr VkImageLayout old_layout = Layout;
    static constexpr auto GetSlice(const TypedImage<Layout, Format>& res) noexcept -> const ImageSlice& {
        return res.Raw();
    }
};

template <VkFormat F>
struct ResourceTraits<RenderTarget<F>> {
    static constexpr VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    static auto GetSlice(const RenderTarget<F>& res) noexcept -> ImageSlice {
        return res.AsSlice();
    }
};

} // namespace TemplatedDetail

// Transition Implementation

template <VkImageLayout TargetLayout, VkFormat F>
[[nodiscard]] constexpr auto Transition(VkCommandBuffer cmd, const RenderTarget<F>& rt, Tag<TargetLayout> /*unused*/) noexcept {
    return TransitionSingle<TargetLayout>(cmd, rt);
}

// Overrides the old loop-based TransitionBatch to issue a single grouped pipeline barrier
template <VkImageLayout TargetLayout, typename... Resources>
[[nodiscard]] constexpr auto TransitionBatch(VkCommandBuffer cmd, const Resources&... resources) noexcept {
    constexpr size_t count = sizeof...(Resources);
    if constexpr (count == 0) {
        return std::tuple<> {};
    } else {
        std::array<VkImageMemoryBarrier2, count> barriers {};
        size_t                                   idx = 0;

        auto populate_barrier = [&](const auto& res) {
            using Traits                       = TemplatedDetail::ResourceTraits<std::decay_t<decltype(res)>>;
            constexpr VkImageLayout old_layout = Traits::old_layout;
            const ImageSlice&                   slice = Traits::GetSlice(res);
            barriers[idx++]                    = MakeImageBarrier(
                MakeLayoutBarrierDesc<old_layout, TargetLayout>(slice.image, slice.aspect)
            );
        };

        (populate_barrier(resources), ...);

        PipelineBarrier(cmd, {}, barriers);

        auto make_typed = [&](const auto& res) {
            using Traits = TemplatedDetail::ResourceTraits<std::decay_t<decltype(res)>>;
            return TypedImage<TargetLayout> {Traits::GetSlice(res)};
        };

        return std::make_tuple(make_typed(resources)...);
    }
}

} // namespace ZHLN::Vk
