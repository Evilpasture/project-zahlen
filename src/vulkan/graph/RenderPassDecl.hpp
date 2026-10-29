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
    static constexpr VkImageLayout         layout = VK_IMAGE_LAYOUT_UNDEFINED;
    static constexpr VkPipelineStageFlags2 stage  = VK_PIPELINE_STAGE_2_NONE;
    static constexpr VkAccessFlags2        access = 0;
};

} // namespace TemplatedDetail

template <>
struct TypeList<> {
    static constexpr size_t size = 0;

    template <size_t I>
    using type = TemplatedDetail::EmptyUsageTag<>;
};

template <
    typename Image, VkImageLayout Layout, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access,
    VkImageLayout FinalLayout = Layout, VkPipelineStageFlags2 FinalStage = Stage, VkAccessFlags2 FinalAccess = Access,
    VkColorComponentFlags ChannelMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT>
struct Usage {
    using Resource                                      = Image;
    static constexpr VkImageLayout         layout       = Layout;
    static constexpr VkPipelineStageFlags2 stage        = Stage;
    static constexpr VkAccessFlags2        access       = Access;
    static constexpr VkImageLayout         final_layout = FinalLayout;
    static constexpr VkPipelineStageFlags2 final_stage  = FinalStage;
    static constexpr VkAccessFlags2        final_access = FinalAccess;
    static constexpr VkColorComponentFlags channel_mask = ChannelMask;
};

template <typename Image>
using ColorWrite = Usage<
    Image,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT>;

// Full Vulkan image synchronization is still required: this mask only
// constrains the pipeline's color writes, not barriers or shader declarations.
template <typename Image>
using ColorWriteRGB = Usage<
    Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT>;

template <typename Image>
using ShaderRead = Usage<Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using DepthWrite = Usage<
    Image,
    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT>;

template <typename Image>
using DepthStencilWrite = Usage<
    Image,
    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT>;

template <typename Image>
using ComputeWrite = Usage<Image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using ComputeRead = Usage<Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using FragmentAndComputeRead = Usage<
    Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using ComputeReadGeneral = Usage<Image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using TransferSrcRead = Usage<Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT>;

template <typename Image>
using TransferDstWrite = Usage<Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT>;

template <typename Image>
using TransferDstWriteThenShaderRead = Usage<
    Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using ShaderReadGeneral = Usage<Image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

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
    typename Image, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access,
    VkImageLayout FinalLayout, VkPipelineStageFlags2 FinalStage, VkAccessFlags2 FinalAccess, VkColorComponentFlags Mask>
struct IsColorAttachment<Usage<Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, Stage, Access, FinalLayout, FinalStage, FinalAccess, Mask>>: std::true_type {};

template <typename U>
struct IsDepthAttachment: std::false_type {};

template <
    typename Image, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access,
    VkImageLayout FinalLayout, VkPipelineStageFlags2 FinalStage, VkAccessFlags2 FinalAccess, VkColorComponentFlags Mask>
struct IsDepthAttachment<Usage<Image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, Stage, Access, FinalLayout, FinalStage, FinalAccess, Mask>>: std::true_type {};

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
