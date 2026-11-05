// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ZHLN::VkD3D pass declarations -- the usage vocabulary a graph pass
// declares its resource hazards in. Everything here is pure compile-time
// data over the neutral enums; the frame graph's state-table analysis
// (StateTable.hpp) and both backend execution layers read it. The Vulkan
// pass execution context a pass body records against lives in
// src/vulkan/graph/PassExecution.hpp.

#include <vkd3d/graph/GraphImage.hpp>

#include <concepts>
#include <cstddef>
#include <string_view>
#include <type_traits>

namespace ZHLN::VkD3D {

template <typename... Ts>
struct TypeList {
    static constexpr size_t size = sizeof...(Ts);

    template <size_t I>
    using type = Ts...[I];
};

template <typename List, typename Target>
struct IsInList;

template <typename... Ts, typename Target>
struct IsInList<TypeList<Ts...>, Target> {
    static constexpr bool value = (std::is_same_v<Ts, Target> || ...);
};

namespace TemplatedDetail {

template <typename = void>
struct EmptyResourceTag {
    static constexpr auto name          = ResourceName("DummyResource");
    static constexpr bool is_swapchain  = false;
    static constexpr bool is_persistent = false;
};

template <typename T = void>
struct EmptyUsageTag {
    using Resource                            = EmptyResourceTag<T>;
    static constexpr auto        name         = ResourceName("DummyResource");
    static constexpr bool        is_swapchain = false;
    static constexpr bool        is_persistent = false;
    static constexpr ImageLayout layout       = ImageLayout::Undefined;
    static constexpr PipelineStage stage      = PipelineStage::None;
    static constexpr Access      access       = Access::None;
};

} // namespace TemplatedDetail

template <>
struct TypeList<> {
    static constexpr size_t size = 0;

    template <size_t I>
    using type = TemplatedDetail::EmptyUsageTag<>;
};

template <
    typename Image, ImageLayout Layout, PipelineStage Stage, Access Access,
    ImageLayout FinalLayout = Layout, PipelineStage FinalStage = Stage, Access FinalAccess = Access,
    ColorMask ChannelMask = ColorMask::Red | ColorMask::Green | ColorMask::Blue | ColorMask::Alpha>
struct Usage {
    using Resource                                = Image;
    static constexpr ImageLayout    layout        = Layout;
    static constexpr PipelineStage  stage         = Stage;
    static constexpr Access         access        = Access;
    static constexpr ImageLayout    final_layout  = FinalLayout;
    static constexpr PipelineStage  final_stage   = FinalStage;
    static constexpr Access         final_access  = FinalAccess;
    static constexpr ColorMask      channel_mask  = ChannelMask;
};

template <typename Image>
using ColorWrite = Usage<
    Image,
    ImageLayout::ColorAttachmentOptimal,
    PipelineStage::ColorAttachment,
    Access::ColorWrite | Access::ColorRead>;

// Full image synchronization is still required: this mask only
// constrains the pipeline's color writes, not barriers or shader declarations.
template <typename Image>
using ColorWriteRGB = Usage<
    Image, ImageLayout::ColorAttachmentOptimal,
    PipelineStage::ColorAttachment,
    Access::ColorWrite | Access::ColorRead,
    ImageLayout::ColorAttachmentOptimal, PipelineStage::ColorAttachment,
    Access::ColorWrite | Access::ColorRead,
    ColorMask::Red | ColorMask::Green | ColorMask::Blue>;

template <typename Image>
using ShaderRead = Usage<Image, ImageLayout::ShaderReadOnlyOptimal, PipelineStage::Fragment, Access::ShaderRead>;

template <typename Image>
using DepthWrite = Usage<
    Image,
    ImageLayout::DepthAttachmentOptimal,
    PipelineStage::EarlyFragment | PipelineStage::LateFragment,
    Access::DepthRead | Access::DepthWrite>;

template <typename Image>
using DepthStencilWrite = Usage<
    Image,
    ImageLayout::DepthStencilAttachmentOptimal,
    PipelineStage::EarlyFragment | PipelineStage::LateFragment,
    Access::DepthRead | Access::DepthWrite>;

template <typename Image>
using ComputeWrite = Usage<Image, ImageLayout::General, PipelineStage::Compute, Access::ShaderWrite | Access::ShaderRead>;

template <typename Image>
using ComputeRead = Usage<Image, ImageLayout::ShaderReadOnlyOptimal, PipelineStage::Compute, Access::ShaderRead>;

template <typename Image>
using FragmentAndComputeRead = Usage<
    Image, ImageLayout::ShaderReadOnlyOptimal,
    PipelineStage::Fragment | PipelineStage::Compute, Access::ShaderRead>;

template <typename Image>
using ComputeReadGeneral = Usage<Image, ImageLayout::General, PipelineStage::Compute, Access::ShaderRead>;

template <typename Image>
using TransferSrcRead = Usage<Image, ImageLayout::TransferSrcOptimal, PipelineStage::Transfer, Access::TransferRead>;

template <typename Image>
using TransferDstWrite = Usage<Image, ImageLayout::TransferDstOptimal, PipelineStage::Transfer, Access::TransferWrite>;

template <typename Image>
using TransferDstWriteThenShaderRead = Usage<
    Image, ImageLayout::TransferDstOptimal, PipelineStage::Transfer, Access::TransferWrite,
    ImageLayout::ShaderReadOnlyOptimal, PipelineStage::Fragment, Access::ShaderRead>;

template <typename Image>
using ShaderReadGeneral = Usage<Image, ImageLayout::General, PipelineStage::Fragment, Access::ShaderRead>;

template <typename T>
concept FrameGraphPass = requires {
    { T::name.string_view() } -> std::convertible_to<std::string_view>;
    typename T::Usages;
};

template <ResourceName Name, typename... UsagesList>
struct RenderPass {
    static constexpr auto name = Name;
    using Usages               = TypeList<UsagesList...>;
};

namespace TemplatedDetail {

template <typename U>
struct IsColorAttachment: std::false_type {};

template <
    typename Image, PipelineStage Stage, Access Access,
    ImageLayout FinalLayout, PipelineStage FinalStage, Access FinalAccess, ColorMask Mask>
struct IsColorAttachment<Usage<Image, ImageLayout::ColorAttachmentOptimal, Stage, Access, FinalLayout, FinalStage, FinalAccess, Mask>>: std::true_type {};

template <typename U>
struct IsDepthAttachment: std::false_type {};

template <
    typename Image, PipelineStage Stage, Access Access,
    ImageLayout FinalLayout, PipelineStage FinalStage, Access FinalAccess, ColorMask Mask>
struct IsDepthAttachment<Usage<Image, ImageLayout::DepthAttachmentOptimal, Stage, Access, FinalLayout, FinalStage, FinalAccess, Mask>>: std::true_type {};

template <
    typename Image, PipelineStage Stage, Access Access,
    ImageLayout FinalLayout, PipelineStage FinalStage, Access FinalAccess, ColorMask Mask>
struct IsDepthAttachment<Usage<Image, ImageLayout::DepthStencilAttachmentOptimal, Stage, Access, FinalLayout, FinalStage, FinalAccess, Mask>>: std::true_type {};

} // namespace TemplatedDetail

// Public graph contract for pipeline setup. Keep internal traversal details in
// TemplatedDetail so render code never depends on its implementation namespace.
template <typename Pass, typename Image>
consteval ColorMask ColorAttachmentWriteMask() {
    return []<typename... Us>(TypeList<Us...>) consteval -> ColorMask {
        constexpr size_t matches = (size_t(std::is_same_v<typename Us::Resource, Image> && TemplatedDetail::IsColorAttachment<Us>::value) + ... + 0u);
        static_assert(matches == 1u, "Pass must declare exactly one color write for this attachment");
        return ((std::is_same_v<typename Us::Resource, Image> && TemplatedDetail::IsColorAttachment<Us>::value ? Us::channel_mask : ColorMask::None) | ... | ColorMask::None);
    }(typename Pass::Usages {});
}

} // namespace ZHLN::VkD3D
