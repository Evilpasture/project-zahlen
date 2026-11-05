// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "GraphImage.hpp"
#include <Zahlen/Core/FunctionRef.hpp>
// The graph vocabulary (TypeList, Usage, RenderPass, FrameGraphPass, forks)
// lives in the joint layer; this header keeps the Vulkan-facing aliases that
// name Vk:: enums and the Vulkan pass context that records commands.
#include <vkd3d/RenderPassDecl.hpp>
#include <concepts>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>

namespace ZHLN::Vk {

// TypeList / IsInList / Usage / RenderPass / FrameGraphPass / the fork protocol
// are the joint layer's vocabulary; re-exported so ZHLN::Vk keeps its spellings
// while the definitions have exactly one home.
using ZHLN::VkD3D::FrameGraphPass;
using ZHLN::VkD3D::IsInList;
using ZHLN::VkD3D::RenderPass;
using ZHLN::VkD3D::ResourceName;
using ZHLN::VkD3D::TypeList;
using ZHLN::VkD3D::Usage;

// The fork protocol is neutral over the command type; these two spellings bind
// it to the Vulkan command buffer so pass code keeps naming Vk::ForkCall /
// Vk::ForkRecorder. Commented as a binding, not a definition: the concept and
// the alias live in the joint layer.
using ForkCall = ZHLN::VkD3D::ForkCall<VkCommandBuffer>;

template <typename Executor>
concept ForkRecorder = ZHLN::VkD3D::ForkRecorder<Executor, VkCommandBuffer>;

using SequentialFork = ZHLN::VkD3D::SequentialFork<VkCommandBuffer>;

static_assert(ForkRecorder<SequentialFork>);

// The declared uses a pass actually names. These are compositions of the
// neutral Usage, which is why they survive the move unchanged: the layout is a
// Vk::ImageLayout and the stage/access are Vk::PipelineStage/Vk::Access, and
// Usage carries them as deduced non-type parameters rather than fixed types.
// A D3D12 pass will compose the same field names with its own enums.
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
using ShaderReadGeneral = Usage<Image, Vk::ImageLayout::General, Vk::PipelineStage::Fragment, Vk::Access::ShaderRead>;

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
using ComputeReadGeneral = Usage<Image, Vk::ImageLayout::General, Vk::PipelineStage::Compute, Vk::Access::ShaderRead>;

template <typename Image>
using FragmentAndComputeRead = Usage<
    Image, Vk::ImageLayout::ShaderReadOnlyOptimal,
    Vk::PipelineStage::Fragment | Vk::PipelineStage::Compute, Vk::Access::ShaderRead>;

template <typename Image>
using TransferSrcRead = Usage<Image, Vk::ImageLayout::TransferSrcOptimal, Vk::PipelineStage::Transfer, Vk::Access::TransferRead>;

template <typename Image>
using TransferDstWrite = Usage<Image, Vk::ImageLayout::TransferDstOptimal, Vk::PipelineStage::Transfer, Vk::Access::TransferWrite>;

template <typename Image>
using TransferDstWriteThenShaderRead = Usage<
    Image, Vk::ImageLayout::TransferDstOptimal, Vk::PipelineStage::Transfer, Vk::Access::TransferWrite,
    Vk::ImageLayout::ShaderReadOnlyOptimal, Vk::PipelineStage::Fragment, Vk::Access::ShaderRead>;

// Everything above is neutral; a Usage's ChannelMask has no Vulkan meaning, but
// the color-write mask a pass hands the pipeline does, so that default stays
// spelled with the VK_COLOR_COMPONENT_* constants below.

namespace TemplatedDetail {

// The graph empty-slot stand-ins moved with the vocabulary; re-exported so the
// Vk:: spellings internal traversal already uses keep resolving.
using ZHLN::VkD3D::TemplatedDetail::EmptyResourceTag;
using ZHLN::VkD3D::TemplatedDetail::EmptyUsageTag;

template <typename U>
struct IsColorAttachment: std::false_type {};

template <
    typename Image, auto Stage, auto Access,
    auto FinalLayout, auto FinalStage, auto FinalAccess, auto Mask>
struct IsColorAttachment<Usage<Image, Vk::ImageLayout::ColorAttachmentOptimal, Stage, Access, FinalLayout, FinalStage, FinalAccess, Mask>>: std::true_type {};

template <typename U>
struct IsDepthAttachment: std::false_type {};

template <
    typename Image, auto Stage, auto Access,
    auto FinalLayout, auto FinalStage, auto FinalAccess, auto Mask>
struct IsDepthAttachment<Usage<Image, Vk::ImageLayout::DepthAttachmentOptimal, Stage, Access, FinalLayout, FinalStage, FinalAccess, Mask>>: std::true_type {};

template <
    typename Image, auto Stage, auto Access,
    auto FinalLayout, auto FinalStage, auto FinalAccess, auto Mask>
struct IsDepthAttachment<Usage<Image, Vk::ImageLayout::DepthStencilAttachmentOptimal, Stage, Access, FinalLayout, FinalStage, FinalAccess, Mask>>: std::true_type {};

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

    // The context opened rendering at the full attachment extent; a pass
    // whose domain viewport differs (the scene viewport) says so, and the
    // context applies viewport and matching scissor itself.
    void SetViewport(const VkViewport& viewport) noexcept {
        const VkRect2D scissor = {
            .offset = {.x = static_cast<int32_t>(viewport.x), .y = static_cast<int32_t>(viewport.y)},
            .extent = {.width = static_cast<uint32_t>(viewport.width), .height = static_cast<uint32_t>(viewport.height)},
        };
        vkCmdSetViewport(m_cmd, 0, 1, &viewport);
        vkCmdSetScissor(m_cmd, 0, 1, &scissor);
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
