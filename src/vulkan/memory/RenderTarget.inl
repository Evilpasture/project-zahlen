#pragma once

#include "RenderTarget.hpp"

namespace ZHLN::Vk {

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

template <typename GraphTag>
struct ResourceTraits<RenderTarget<GraphTag>> {
    static constexpr VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    static auto GetSlice(const RenderTarget<GraphTag>& res) noexcept -> ImageSlice {
        return res.AsSlice();
    }
};

}

template <VkImageLayout TargetLayout, typename GraphTag>
[[nodiscard]] constexpr auto Transition(VkCommandBuffer cmd, const RenderTarget<GraphTag>& rt, Tag<TargetLayout> ) noexcept {
    return TransitionSingle<TargetLayout>(cmd, rt);
}

template <VkImageLayout TargetLayout, typename... Resources>
[[nodiscard]] constexpr auto TransitionBatch(VkCommandBuffer cmd, const Resources&... resources) noexcept {
    constexpr size_t count = sizeof...(Resources);
    if constexpr (count == 0) {
        return std::tuple<> {};
    } else {
        std::array<ImageBarrier, count> barriers {};
        size_t                          idx = 0;

        auto populate_barrier = [&](const auto& res) {
            using Traits                       = TemplatedDetail::ResourceTraits<std::decay_t<decltype(res)>>;
            constexpr VkImageLayout old_layout = Traits::old_layout;
            const ImageSlice        slice      = Traits::GetSlice(res);
            barriers[idx++]                    = ImageBarrier::Transition<old_layout, TargetLayout>(slice);
        };

        (populate_barrier(resources), ...);

        ImageBarrier::Record(cmd, barriers);

        auto make_typed = [&](const auto& res) {
            using Traits = TemplatedDetail::ResourceTraits<std::decay_t<decltype(res)>>;
            return TypedImage<TargetLayout> {Traits::GetSlice(res)};
        };

        return std::make_tuple(make_typed(resources)...);
    }
}

}
