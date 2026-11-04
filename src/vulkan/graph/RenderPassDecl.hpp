// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "GraphImage.hpp"
#include <Zahlen/Core/FunctionRef.hpp>
#include <concepts>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>

namespace ZHLN::Vk {

template <typename... Ts>
struct TypeList {
    static constexpr size_t size = sizeof...(Ts);

    template <size_t I>
    using type = Ts...[I];
};

template <typename List, typename Target>
struct IsInList;

template <typename... Ts, typename Target>
struct IsInList<Vk::TypeList<Ts...>, Target> {
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
    using Resource                                = EmptyResourceTag<T>;
    static constexpr auto                  name   = ResourceName("DummyResource");
    static constexpr bool                  is_swapchain  = false;
    static constexpr bool                  is_persistent = false;
    static constexpr Vk::ImageLayout         layout = Vk::ImageLayout::Undefined;
    static constexpr Vk::PipelineStage stage  = Vk::PipelineStage::StageNone;
    static constexpr Vk::Access        access = Vk::Access::AccessNone;
};

} // namespace TemplatedDetail

template <>
struct TypeList<> {
    static constexpr size_t size = 0;

    template <size_t I>
    using type = TemplatedDetail::EmptyUsageTag<>;
};

template <
    typename Image, Vk::ImageLayout Layout, Vk::PipelineStage Stage, Vk::Access Access,
    Vk::ImageLayout FinalLayout = Layout, Vk::PipelineStage FinalStage = Stage, Vk::Access FinalAccess = Access,
    VkColorComponentFlags ChannelMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT>
struct Usage {
    using Resource                                      = Image;
    static constexpr Vk::ImageLayout         layout       = Layout;
    static constexpr Vk::PipelineStage stage        = Stage;
    static constexpr Vk::Access        access       = Access;
    static constexpr Vk::ImageLayout         final_layout = FinalLayout;
    static constexpr Vk::PipelineStage final_stage  = FinalStage;
    static constexpr Vk::Access        final_access = FinalAccess;
    static constexpr VkColorComponentFlags channel_mask = ChannelMask;
};

template <typename Image>
using ColorWrite = Usage<
    Image,
    Vk::ImageLayout::ColorAttachmentOptimal,
    Vk::PipelineStage::ColorAttachment,
    Vk::Access::ColorWrite | Vk::Access::ColorRead>;

// Full Vulkan image synchronization is still required: this mask only
// constrains the pipeline's color writes, not barriers or shader declarations.
template <typename Image>
using ColorWriteRGB = Usage<
    Image, Vk::ImageLayout::ColorAttachmentOptimal,
    Vk::PipelineStage::ColorAttachment,
    Vk::Access::ColorWrite | Vk::Access::ColorRead,
    Vk::ImageLayout::ColorAttachmentOptimal, Vk::PipelineStage::ColorAttachment,
    Vk::Access::ColorWrite | Vk::Access::ColorRead,
    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT>;

template <typename Image>
using ShaderRead = Usage<Image, Vk::ImageLayout::ShaderReadOnlyOptimal, Vk::PipelineStage::Fragment, Vk::Access::ShaderRead>;

template <typename Image>
using DepthWrite = Usage<
    Image,
    Vk::ImageLayout::DepthAttachmentOptimal,
    Vk::PipelineStage::EarlyFragment | Vk::PipelineStage::LateFragment,
    Vk::Access::DepthRead | Vk::Access::DepthWrite>;

template <typename Image>
using DepthStencilWrite = Usage<
    Image,
    Vk::ImageLayout::DepthStencilAttachmentOptimal,
    Vk::PipelineStage::EarlyFragment | Vk::PipelineStage::LateFragment,
    Vk::Access::DepthRead | Vk::Access::DepthWrite>;

template <typename Image>
using ComputeWrite = Usage<Image, Vk::ImageLayout::General, Vk::PipelineStage::Compute, Vk::Access::ShaderWrite | Vk::Access::ShaderRead>;

template <typename Image>
using ComputeRead = Usage<Image, Vk::ImageLayout::ShaderReadOnlyOptimal, Vk::PipelineStage::Compute, Vk::Access::ShaderRead>;

template <typename Image>
using FragmentAndComputeRead = Usage<
    Image, Vk::ImageLayout::ShaderReadOnlyOptimal,
    Vk::PipelineStage::Fragment | Vk::PipelineStage::Compute, Vk::Access::ShaderRead>;

template <typename Image>
using ComputeReadGeneral = Usage<Image, Vk::ImageLayout::General, Vk::PipelineStage::Compute, Vk::Access::ShaderRead>;

template <typename Image>
using TransferSrcRead = Usage<Image, Vk::ImageLayout::TransferSrcOptimal, Vk::PipelineStage::Transfer, Vk::Access::TransferRead>;

template <typename Image>
using TransferDstWrite = Usage<Image, Vk::ImageLayout::TransferDstOptimal, Vk::PipelineStage::Transfer, Vk::Access::TransferWrite>;

template <typename Image>
using TransferDstWriteThenShaderRead = Usage<
    Image, Vk::ImageLayout::TransferDstOptimal, Vk::PipelineStage::Transfer, Vk::Access::TransferWrite,
    Vk::ImageLayout::ShaderReadOnlyOptimal, Vk::PipelineStage::Fragment, Vk::Access::ShaderRead>;

template <typename Image>
using ShaderReadGeneral = Usage<Image, Vk::ImageLayout::General, Vk::PipelineStage::Fragment, Vk::Access::ShaderRead>;

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

using ForkCall = ZHLN::FunctionRef<void(VkCommandBuffer) const>;

template <typename Executor>
concept ForkRecorder = requires(Executor& executor, VkCommandBuffer cmd, std::span<const ForkCall> bodies) {
    { executor.ExecuteFork(cmd, bodies) } noexcept;
};

struct SequentialFork {
    static constexpr void ExecuteFork(VkCommandBuffer cmd, std::span<const ForkCall> bodies) noexcept {
        for (const ForkCall& body : bodies) {
            body(cmd);
        }
    }
};

static_assert(ForkRecorder<SequentialFork>);

namespace TemplatedDetail {

template <typename U>
struct IsColorAttachment: std::false_type {};

template <
    typename Image, Vk::PipelineStage Stage, Vk::Access Access,
    Vk::ImageLayout FinalLayout, Vk::PipelineStage FinalStage, Vk::Access FinalAccess, VkColorComponentFlags Mask>
struct IsColorAttachment<Usage<Image, Vk::ImageLayout::ColorAttachmentOptimal, Stage, Access, FinalLayout, FinalStage, FinalAccess, Mask>>: std::true_type {};

template <typename U>
struct IsDepthAttachment: std::false_type {};

template <
    typename Image, Vk::PipelineStage Stage, Vk::Access Access,
    Vk::ImageLayout FinalLayout, Vk::PipelineStage FinalStage, Vk::Access FinalAccess, VkColorComponentFlags Mask>
struct IsDepthAttachment<Usage<Image, Vk::ImageLayout::DepthAttachmentOptimal, Stage, Access, FinalLayout, FinalStage, FinalAccess, Mask>>: std::true_type {};

} // namespace TemplatedDetail

// Public graph contract for pipeline setup. Keep internal traversal details in
// TemplatedDetail so render code never depends on its implementation namespace.
template <typename Pass, typename Image>
consteval VkColorComponentFlags ColorAttachmentWriteMask() {
    return []<typename... Us>(TypeList<Us...>) consteval -> VkColorComponentFlags {
        constexpr size_t matches = (size_t(std::is_same_v<typename Us::Resource, Image> && TemplatedDetail::IsColorAttachment<Us>::value) + ... + 0u);
        static_assert(matches == 1u, "Pass must declare exactly one color write for this attachment");
        return ((std::is_same_v<typename Us::Resource, Image> && TemplatedDetail::IsColorAttachment<Us>::value ? Us::channel_mask : 0u) | ... | 0u);
    }(typename Pass::Usages {});
}

class RasterPassContextBase {
  public:
    explicit RasterPassContextBase(VkCommandBuffer cmd) noexcept: m_cmd(cmd) {
    }
    ~RasterPassContextBase() = default;

    RasterPassContextBase(const RasterPassContextBase&)                        = delete;
    auto operator=(const RasterPassContextBase&) -> RasterPassContextBase&     = delete;
    RasterPassContextBase(RasterPassContextBase&&) noexcept                    = delete;
    auto operator=(RasterPassContextBase&&) noexcept -> RasterPassContextBase& = delete;

    [[nodiscard]] auto Cmd() const noexcept -> VkCommandBuffer {
        return m_cmd;
    }
    [[nodiscard]] auto Extent() const noexcept -> VkExtent2D {
        return m_extent;
    }

  protected:
    void SetExtent(VkExtent2D extent) noexcept {
        m_extent = extent;
    }

  private:
    VkCommandBuffer m_cmd;
    VkExtent2D      m_extent {};
};

} // namespace ZHLN::Vk
