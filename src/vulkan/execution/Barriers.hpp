// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../memory/BufferSlice.hpp"
#include "../memory/ImageSlice.hpp"
#include "../memory/ImageView.hpp"
#include "RenderQueue.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ZHLN::Vk {

struct LayoutSyncInfo {
    VkPipelineStageFlags2 stage;
    VkAccessFlags2        access;
    std::string_view      name;
};

[[nodiscard]] constexpr auto GetLayoutSyncInfo(VkImageLayout layout, bool isSource) noexcept -> LayoutSyncInfo {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            return {.stage = VK_PIPELINE_STAGE_2_NONE, .access = 0, .name = "UNDEFINED"};

        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return {
                .stage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .access = isSource ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
                .name   = "COLOR_ATTACHMENT_OPTIMAL",
            };

        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
            return {
                .stage  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                .access = isSource ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT :
                                     (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
                .name   = "DEPTH_ATTACHMENT_OPTIMAL",
            };

        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return {
                .stage  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                .access = isSource ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT :
                                     (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
                .name   = "DEPTH_STENCIL_ATTACHMENT_OPTIMAL",
            };

        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return {
                .stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .access = VK_ACCESS_2_SHADER_READ_BIT,
                .name   = "SHADER_READ_ONLY_OPTIMAL",
            };

        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            return {.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .access = 0, .name = "PRESENT_SRC_KHR"};

        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return {.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT, .name = "TRANSFER_DST_OPTIMAL"};

        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            return {.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_READ_BIT, .name = "TRANSFER_SRC_OPTIMAL"};

        case VK_IMAGE_LAYOUT_GENERAL:
            return {
                .stage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .access = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                .name   = "GENERAL",
            };

        default:
            return {
                .stage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .access = isSource ? VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_MEMORY_READ_BIT,
                .name   = "UNKNOWN_OR_CUSTOM_LAYOUT",
            };
    }
}

template <VkImageLayout Layout>
struct LayoutTraits {
    static constexpr auto             kInfo = GetLayoutSyncInfo(Layout, false);
    static constexpr std::string_view kName = kInfo.name;

    static constexpr VkPipelineStageFlags2 kStage = []() constexpr {
        if constexpr (Layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            return VK_PIPELINE_STAGE_2_NONE;
        } else if constexpr (Layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        } else if constexpr (Layout == VK_IMAGE_LAYOUT_GENERAL) {
            return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        } else {
            return kInfo.stage;
        }
    }();

    static constexpr VkAccessFlags2 kAccess = kInfo.access;
};

// A compact 64-byte (one cache line) value-type description of an image memory
// barrier, following the same JIT synthesis discipline as Vk::Sampler,
// Vk::SemaphoreSubmit, and Vk::ImageView.
//
// Callers state semantic intent from rich domain objects (ImageView, ImageSlice,
// RenderTarget) rather than unpacking raw handles, aspect masks, and subresource
// ranges into a 96-byte VkImageMemoryBarrier2 at every call site; the Vulkan C
// struct is synthesized strictly on the stack where vkCmdPipelineBarrier2
// consumes it.
class ImageBarrier {
  public:
    static constexpr uint16_t kRemainingMips = 0xFFFFu;

    constexpr ImageBarrier() noexcept = default;

    // -------------------------------------------------------------------------
    // Semantic "Tell, Don't Ask" Factories from Domain Objects
    // -------------------------------------------------------------------------

    /// Automatic layout transition for any ImageView (deduces aspect, subresource range, stages, and accesses).
    template <VkImageLayout OldLayout, VkImageLayout NewLayout>
    [[nodiscard]] static constexpr auto Transition(const ImageView& view) noexcept -> ImageBarrier {
        using Src = LayoutTraits<OldLayout>;
        using Dst = LayoutTraits<NewLayout>;
        return FromRange(view.Handle(), view.SubresourceRange(), OldLayout, NewLayout, Src::kStage, Src::kAccess, Dst::kStage, Dst::kAccess);
    }

    /// Automatic layout transition for an ImageSlice.
    template <VkImageLayout OldLayout, VkImageLayout NewLayout>
    [[nodiscard]] static constexpr auto Transition(const ImageSlice& slice) noexcept -> ImageBarrier {
        return Transition<OldLayout, NewLayout>(slice.View());
    }

    /// Automatic layout transition for an explicit subresource range when only a raw VkImage handle is available.
    template <VkImageLayout OldLayout, VkImageLayout NewLayout>
    [[nodiscard]] static constexpr auto Transition(VkImage image, const VkImageSubresourceRange& range) noexcept -> ImageBarrier {
        using Src = LayoutTraits<OldLayout>;
        using Dst = LayoutTraits<NewLayout>;
        return FromRange(image, range, OldLayout, NewLayout, Src::kStage, Src::kAccess, Dst::kStage, Dst::kAccess);
    }

    /// Runtime layout transition deriving conservative sync masks from oldLayout / newLayout.
    [[nodiscard]] static constexpr auto Transition(const ImageView& view, VkImageLayout oldLayout, VkImageLayout newLayout) noexcept -> ImageBarrier {
        const LayoutSyncInfo src = GetLayoutSyncInfo(oldLayout, true);
        const LayoutSyncInfo dst = GetLayoutSyncInfo(newLayout, false);
        return FromRange(view.Handle(), view.SubresourceRange(), oldLayout, newLayout, src.stage, src.access, dst.stage, dst.access);
    }

    [[nodiscard]] static constexpr auto Transition(const ImageSlice& slice, VkImageLayout oldLayout, VkImageLayout newLayout) noexcept -> ImageBarrier {
        return Transition(slice.View(), oldLayout, newLayout);
    }

    /// Transition with explicit stage/access override (e.g. non-default pipeline stages or frame-graph state tables).
    [[nodiscard]] static constexpr auto Transition(
        const ImageView&      view,
        VkImageLayout         oldLayout,
        VkImageLayout         newLayout,
        VkPipelineStageFlags2 srcStage,
        VkAccessFlags2        srcAccess,
        VkPipelineStageFlags2 dstStage,
        VkAccessFlags2        dstAccess
    ) noexcept -> ImageBarrier {
        return FromRange(view.Handle(), view.SubresourceRange(), oldLayout, newLayout, srcStage, srcAccess, dstStage, dstAccess);
    }

    [[nodiscard]] static constexpr auto Transition(
        const ImageSlice&     slice,
        VkImageLayout         oldLayout,
        VkImageLayout         newLayout,
        VkPipelineStageFlags2 srcStage,
        VkAccessFlags2        srcAccess,
        VkPipelineStageFlags2 dstStage,
        VkAccessFlags2        dstAccess
    ) noexcept -> ImageBarrier {
        return Transition(slice.View(), oldLayout, newLayout, srcStage, srcAccess, dstStage, dstAccess);
    }

    [[nodiscard]] static constexpr auto Transition(
        VkImage                        image,
        const VkImageSubresourceRange& range,
        VkImageLayout                  oldLayout,
        VkImageLayout                  newLayout,
        VkPipelineStageFlags2          srcStage,
        VkAccessFlags2                 srcAccess,
        VkPipelineStageFlags2          dstStage,
        VkAccessFlags2                 dstAccess
    ) noexcept -> ImageBarrier {
        return FromRange(image, range, oldLayout, newLayout, srcStage, srcAccess, dstStage, dstAccess);
    }

    /// Specialized factory for swapchain presentation handoff.
    [[nodiscard]] static constexpr auto PreparePresent(VkImage swapchainImage, VkImageLayout currentLayout) noexcept -> ImageBarrier {
        return FromRange(
            swapchainImage,
            VkImageSubresourceRange {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
            currentLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0
        );
    }

    /// Discard existing contents (VK_IMAGE_LAYOUT_UNDEFINED -> TargetLayout).
    template <VkImageLayout TargetLayout>
    [[nodiscard]] static constexpr auto DiscardTo(const ImageView& view) noexcept -> ImageBarrier {
        return Transition<VK_IMAGE_LAYOUT_UNDEFINED, TargetLayout>(view);
    }

    template <VkImageLayout TargetLayout>
    [[nodiscard]] static constexpr auto DiscardTo(const ImageSlice& slice) noexcept -> ImageBarrier {
        return DiscardTo<TargetLayout>(slice.View());
    }

    template <VkImageLayout TargetLayout>
    [[nodiscard]] static constexpr auto DiscardTo(VkImage image, const VkImageSubresourceRange& range) noexcept -> ImageBarrier {
        return Transition<VK_IMAGE_LAYOUT_UNDEFINED, TargetLayout>(image, range);
    }

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return _image != VK_NULL_HANDLE;
    }

    // -------------------------------------------------------------------------
    // Execution ("Tell the barrier to record")
    // -------------------------------------------------------------------------

    void Record(VkCommandBuffer cmd) const noexcept {
        if (cmd == VK_NULL_HANDLE || _image == VK_NULL_HANDLE) {
            return;
        }
        const VkImageMemoryBarrier2 barrier = AsVkBarrier();
        const VkDependencyInfo      dep {
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers    = &barrier,
        };
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    /// Zero-allocation stack submission of a fixed-size barrier batch.
    template <size_t N>
    static void Record(VkCommandBuffer cmd, const std::array<ImageBarrier, N>& barriers) noexcept {
        if constexpr (N == 0) {
            return;
        } else {
            if (cmd == VK_NULL_HANDLE) {
                return;
            }
            std::array<VkImageMemoryBarrier2, N> vkBarriers {};
            for (size_t i = 0; i < N; ++i) {
                vkBarriers[i] = barriers[i].AsVkBarrier();
            }
            const VkDependencyInfo dep {
                .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = static_cast<uint32_t>(N),
                .pImageMemoryBarriers    = vkBarriers.data(),
            };
            vkCmdPipelineBarrier2(cmd, &dep);
        }
    }

    /// Dynamic-count batch submission (stack-allocated up to 16 barriers).
    static void Record(VkCommandBuffer cmd, std::span<const ImageBarrier> barriers) noexcept {
        if (barriers.empty() || cmd == VK_NULL_HANDLE) {
            return;
        }

        constexpr size_t kStackLimit = 16;
        if (barriers.size() <= kStackLimit) {
            std::array<VkImageMemoryBarrier2, kStackLimit> vkBarriers {};
            for (size_t i = 0; i < barriers.size(); ++i) {
                vkBarriers[i] = barriers[i].AsVkBarrier();
            }
            const VkDependencyInfo dep {
                .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
                .pImageMemoryBarriers    = vkBarriers.data(),
            };
            vkCmdPipelineBarrier2(cmd, &dep);
        } else {
            std::vector<VkImageMemoryBarrier2> heapBarriers;
            heapBarriers.reserve(barriers.size());
            for (const auto& b: barriers) {
                heapBarriers.push_back(b.AsVkBarrier());
            }
            const VkDependencyInfo dep {
                .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = static_cast<uint32_t>(heapBarriers.size()),
                .pImageMemoryBarriers    = heapBarriers.data(),
            };
            vkCmdPipelineBarrier2(cmd, &dep);
        }
    }

    // -------------------------------------------------------------------------
    // JIT Stack Synthesis
    // -------------------------------------------------------------------------
    [[nodiscard]] constexpr auto AsVkBarrier() const noexcept -> VkImageMemoryBarrier2 {
        return {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext               = nullptr,
            .srcStageMask        = _srcStage,
            .srcAccessMask       = _srcAccess,
            .dstStageMask        = _dstStage,
            .dstAccessMask       = _dstAccess,
            .oldLayout           = _oldLayout,
            .newLayout           = _newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = _image,
            .subresourceRange    = {
                .aspectMask     = _aspect,
                .baseMipLevel   = _baseMipLevel,
                .levelCount     = _levelCount == kRemainingMips ? VK_REMAINING_MIP_LEVELS : static_cast<uint32_t>(_levelCount),
                .baseArrayLayer = _baseArrayLayer,
                .layerCount     = _layerCount,
            },
        };
    }

  private:
    [[nodiscard]] static constexpr auto FromRange(
        VkImage                       image,
        const VkImageSubresourceRange range,
        VkImageLayout                 oldLayout,
        VkImageLayout                 newLayout,
        VkPipelineStageFlags2         srcStage,
        VkAccessFlags2                srcAccess,
        VkPipelineStageFlags2         dstStage,
        VkAccessFlags2                dstAccess
    ) noexcept -> ImageBarrier {
        ImageBarrier b {};
        b._image          = image;
        b._srcStage       = srcStage;
        b._srcAccess      = srcAccess;
        b._dstStage       = dstStage;
        b._dstAccess      = dstAccess;
        b._oldLayout      = oldLayout;
        b._newLayout      = newLayout;
        b._aspect         = range.aspectMask;
        b._baseArrayLayer = range.baseArrayLayer;
        b._layerCount     = range.layerCount;
        b._baseMipLevel   = static_cast<uint16_t>(range.baseMipLevel);
        b._levelCount     = range.levelCount == VK_REMAINING_MIP_LEVELS ? kRemainingMips : static_cast<uint16_t>(range.levelCount);
        return b;
    }

    VkImage               _image          = VK_NULL_HANDLE;            // 8 bytes
    VkPipelineStageFlags2 _srcStage       = VK_PIPELINE_STAGE_2_NONE;  // 8 bytes
    VkAccessFlags2        _srcAccess      = 0;                         // 8 bytes
    VkPipelineStageFlags2 _dstStage       = VK_PIPELINE_STAGE_2_NONE;  // 8 bytes
    VkAccessFlags2        _dstAccess      = 0;                         // 8 bytes
    VkImageLayout         _oldLayout      = VK_IMAGE_LAYOUT_UNDEFINED; // 4 bytes
    VkImageLayout         _newLayout      = VK_IMAGE_LAYOUT_UNDEFINED; // 4 bytes
    VkImageAspectFlags    _aspect         = VK_IMAGE_ASPECT_COLOR_BIT; // 4 bytes
    uint32_t              _baseArrayLayer = 0;                         // 4 bytes
    uint32_t              _layerCount     = VK_REMAINING_ARRAY_LAYERS; // 4 bytes
    uint16_t              _baseMipLevel   = 0;                         // 2 bytes
    uint16_t              _levelCount     = kRemainingMips;            // 2 bytes
};

static_assert(sizeof(ImageBarrier) == 64, "ImageBarrier must fit in a single 64-byte cache line.");
static_assert(std::is_trivially_copyable_v<ImageBarrier>);

// A compact 32-byte value-type description of a global memory/execution barrier.
// Callers name the hazard they want to resolve via semantic factories; the
// 48-byte VkMemoryBarrier2 is synthesized on the stack at Record() time.
class MemoryBarrier {
  public:
    constexpr MemoryBarrier() noexcept = default;

    constexpr MemoryBarrier(VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) noexcept:
        _srcStage(srcStage), _srcAccess(srcAccess), _dstStage(dstStage), _dstAccess(dstAccess) {
    }

    constexpr MemoryBarrier(BarrierStage srcStage, BarrierAccess srcAccess, BarrierStage dstStage, BarrierAccess dstAccess) noexcept:
        _srcStage(static_cast<VkPipelineStageFlags2>(srcStage)), _srcAccess(static_cast<VkAccessFlags2>(srcAccess)),
        _dstStage(static_cast<VkPipelineStageFlags2>(dstStage)), _dstAccess(static_cast<VkAccessFlags2>(dstAccess)) {
    }

    // -------------------------------------------------------------------------
    // Semantic Intent Factories ("Ask for the hazard you want to resolve")
    // -------------------------------------------------------------------------

    /// Compute shader write to compute shader read (e.g. mip pyramids, Kawase bloom chains).
    [[nodiscard]] static constexpr auto ComputeWriteToRead() noexcept -> MemoryBarrier {
        return {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
        };
    }

    /// Compute shader write to indirect draw argument consumption (e.g. GPU frustum/HiZ culling).
    [[nodiscard]] static constexpr auto ComputeToIndirect() noexcept -> MemoryBarrier {
        return {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
        };
    }

    /// Compute write to subsequent vertex/fragment shader reads.
    [[nodiscard]] static constexpr auto ComputeToShaderRead() noexcept -> MemoryBarrier {
        return {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
        };
    }

    /// Transfer write to indirect draw argument consumption.
    [[nodiscard]] static constexpr auto TransferToIndirect() noexcept -> MemoryBarrier {
        return {
            VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
        };
    }

    /// Transfer/copy write to subsequent indirect/index/shader/uniform graphics reads.
    [[nodiscard]] static constexpr auto TransferToGraphics() noexcept -> MemoryBarrier {
        return {
            VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT,
        };
    }

    /// Serialize overlapping staged copy writes targeting the same destination buffer.
    [[nodiscard]] static constexpr auto CopyWriteToWrite() noexcept -> MemoryBarrier {
        return {
            VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
        };
    }

    /// Staged buffer copy write to BLAS/TLAS build geometry input read.
    [[nodiscard]] static constexpr auto TransferToAccelerationStructureBuild() noexcept -> MemoryBarrier {
        return {
            VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
        };
    }

    /// BLAS build write to subsequent TLAS build read.
    [[nodiscard]] static constexpr auto AccelerationStructureBuildToBuild() noexcept -> MemoryBarrier {
        return {
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
        };
    }

    /// TLAS build write to ray-query / trace reads in fragment and compute shaders.
    [[nodiscard]] static constexpr auto AccelerationStructureBuildToTrace() noexcept -> MemoryBarrier {
        return {
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
        };
    }

    // -------------------------------------------------------------------------
    // Execution ("Tell the barrier to record")
    // -------------------------------------------------------------------------
    void Record(VkCommandBuffer cmd) const noexcept {
        if (cmd == VK_NULL_HANDLE) {
            return;
        }
        const VkMemoryBarrier2 mem = AsVkBarrier();
        const VkDependencyInfo dep {
            .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers    = &mem,
        };
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    template <size_t N>
    static void Record(VkCommandBuffer cmd, const std::array<MemoryBarrier, N>& barriers) noexcept {
        if constexpr (N == 0) {
            return;
        } else {
            if (cmd == VK_NULL_HANDLE) {
                return;
            }
            std::array<VkMemoryBarrier2, N> vkBarriers {};
            for (size_t i = 0; i < N; ++i) {
                vkBarriers[i] = barriers[i].AsVkBarrier();
            }
            const VkDependencyInfo dep {
                .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = static_cast<uint32_t>(N),
                .pMemoryBarriers    = vkBarriers.data(),
            };
            vkCmdPipelineBarrier2(cmd, &dep);
        }
    }

    static void Record(VkCommandBuffer cmd, std::span<const MemoryBarrier> barriers) noexcept {
        if (barriers.empty() || cmd == VK_NULL_HANDLE) {
            return;
        }
        constexpr size_t kStackLimit = 8;
        if (barriers.size() <= kStackLimit) {
            std::array<VkMemoryBarrier2, kStackLimit> vkBarriers {};
            for (size_t i = 0; i < barriers.size(); ++i) {
                vkBarriers[i] = barriers[i].AsVkBarrier();
            }
            const VkDependencyInfo dep {
                .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = static_cast<uint32_t>(barriers.size()),
                .pMemoryBarriers    = vkBarriers.data(),
            };
            vkCmdPipelineBarrier2(cmd, &dep);
        } else {
            std::vector<VkMemoryBarrier2> heapBarriers;
            heapBarriers.reserve(barriers.size());
            for (const auto& b: barriers) {
                heapBarriers.push_back(b.AsVkBarrier());
            }
            const VkDependencyInfo dep {
                .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = static_cast<uint32_t>(heapBarriers.size()),
                .pMemoryBarriers    = heapBarriers.data(),
            };
            vkCmdPipelineBarrier2(cmd, &dep);
        }
    }

    [[nodiscard]] constexpr auto AsVkBarrier() const noexcept -> VkMemoryBarrier2 {
        return {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .pNext         = nullptr,
            .srcStageMask  = _srcStage,
            .srcAccessMask = _srcAccess,
            .dstStageMask  = _dstStage,
            .dstAccessMask = _dstAccess,
        };
    }

  private:
    VkPipelineStageFlags2 _srcStage  = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        _srcAccess = 0;
    VkPipelineStageFlags2 _dstStage  = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        _dstAccess = 0;
};

static_assert(sizeof(MemoryBarrier) == 32, "MemoryBarrier must be exactly 32 bytes.");
static_assert(std::is_trivially_copyable_v<MemoryBarrier>);

class Buffer;

// =============================================================================
// BufferBarrier (64 bytes, 1 cache line)
// =============================================================================
// First-class value type describing a buffer or sub-buffer synchronization
// hazard and optional queue-family ownership transfer.
//
// `VkBufferMemoryBarrier2` is 80 bytes with sType/pNext overhead. `BufferBarrier`
// fits in a single 64-byte L1 cache line and synthesizes the C ABI struct on the
// stack immediately where `vkCmdPipelineBarrier2` consumes it.
class BufferBarrier {
  public:
    constexpr BufferBarrier() noexcept = default;

    constexpr BufferBarrier(
        VkBuffer              buffer,
        VkPipelineStageFlags2 srcStage,
        VkAccessFlags2        srcAccess,
        VkPipelineStageFlags2 dstStage,
        VkAccessFlags2        dstAccess,
        VkDeviceSize          offset         = 0,
        VkDeviceSize          size           = VK_WHOLE_SIZE,
        uint32_t              srcQueueFamily = VK_QUEUE_FAMILY_IGNORED,
        uint32_t              dstQueueFamily = VK_QUEUE_FAMILY_IGNORED
    ) noexcept:
        _buffer(buffer), _offset(offset), _size(size), _srcStage(srcStage), _srcAccess(srcAccess), _dstStage(dstStage), _dstAccess(dstAccess),
        _srcQueueFamily(srcQueueFamily), _dstQueueFamily(dstQueueFamily) {
    }

    constexpr BufferBarrier(
        VkBuffer      buffer,
        BarrierStage  srcStage,
        BarrierAccess srcAccess,
        BarrierStage  dstStage,
        BarrierAccess dstAccess,
        VkDeviceSize  offset         = 0,
        VkDeviceSize  size           = VK_WHOLE_SIZE,
        uint32_t      srcQueueFamily = VK_QUEUE_FAMILY_IGNORED,
        uint32_t      dstQueueFamily = VK_QUEUE_FAMILY_IGNORED
    ) noexcept:
        BufferBarrier(
            buffer,
            static_cast<VkPipelineStageFlags2>(srcStage),
            static_cast<VkAccessFlags2>(srcAccess),
            static_cast<VkPipelineStageFlags2>(dstStage),
            static_cast<VkAccessFlags2>(dstAccess),
            offset,
            size,
            srcQueueFamily,
            dstQueueFamily
        ) {
    }

    constexpr BufferBarrier(
        BufferSlice           slice,
        VkPipelineStageFlags2 srcStage,
        VkAccessFlags2        srcAccess,
        VkPipelineStageFlags2 dstStage,
        VkAccessFlags2        dstAccess,
        uint32_t              srcQueueFamily = VK_QUEUE_FAMILY_IGNORED,
        uint32_t              dstQueueFamily = VK_QUEUE_FAMILY_IGNORED
    ) noexcept: BufferBarrier(slice.buffer, srcStage, srcAccess, dstStage, dstAccess, slice.offset, slice.size, srcQueueFamily, dstQueueFamily) {
    }

    constexpr BufferBarrier(
        BufferSlice   slice,
        BarrierStage  srcStage,
        BarrierAccess srcAccess,
        BarrierStage  dstStage,
        BarrierAccess dstAccess,
        uint32_t      srcQueueFamily = VK_QUEUE_FAMILY_IGNORED,
        uint32_t      dstQueueFamily = VK_QUEUE_FAMILY_IGNORED
    ) noexcept:
        BufferBarrier(
            slice.buffer,
            static_cast<VkPipelineStageFlags2>(srcStage),
            static_cast<VkAccessFlags2>(srcAccess),
            static_cast<VkPipelineStageFlags2>(dstStage),
            static_cast<VkAccessFlags2>(dstAccess),
            slice.offset,
            slice.size,
            srcQueueFamily,
            dstQueueFamily
        ) {
    }

    BufferBarrier(
        const Buffer&         buffer,
        VkPipelineStageFlags2 srcStage,
        VkAccessFlags2        srcAccess,
        VkPipelineStageFlags2 dstStage,
        VkAccessFlags2        dstAccess,
        VkDeviceSize          offset         = 0,
        VkDeviceSize          size           = VK_WHOLE_SIZE,
        uint32_t              srcQueueFamily = VK_QUEUE_FAMILY_IGNORED,
        uint32_t              dstQueueFamily = VK_QUEUE_FAMILY_IGNORED
    ) noexcept;

    BufferBarrier(
        const Buffer& buffer,
        BarrierStage  srcStage,
        BarrierAccess srcAccess,
        BarrierStage  dstStage,
        BarrierAccess dstAccess,
        VkDeviceSize  offset         = 0,
        VkDeviceSize  size           = VK_WHOLE_SIZE,
        uint32_t      srcQueueFamily = VK_QUEUE_FAMILY_IGNORED,
        uint32_t      dstQueueFamily = VK_QUEUE_FAMILY_IGNORED
    ) noexcept;

    // -------------------------------------------------------------------------
    // Semantic Intent Factories
    // -------------------------------------------------------------------------

    /// Transfer/clear write (`vkCmdFillBuffer`, `vkCmdCopyBuffer2`) to compute shader read/write.
    [[nodiscard]] static constexpr auto
        TransferToCompute(VkBuffer buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept -> BufferBarrier {
        return {
            buffer,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            offset,
            size,
        };
    }

    [[nodiscard]] static constexpr auto TransferToCompute(BufferSlice slice) noexcept -> BufferBarrier {
        return TransferToCompute(slice.buffer, slice.offset, slice.size);
    }

    [[nodiscard]] static auto TransferToCompute(const Buffer& buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept -> BufferBarrier;

    /// Compute shader read/write to subsequent transfer/clear write.
    [[nodiscard]] static constexpr auto
        ComputeToTransfer(VkBuffer buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept -> BufferBarrier {
        return {
            buffer,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            offset,
            size,
        };
    }

    [[nodiscard]] static constexpr auto ComputeToTransfer(BufferSlice slice) noexcept -> BufferBarrier {
        return ComputeToTransfer(slice.buffer, slice.offset, slice.size);
    }

    [[nodiscard]] static auto ComputeToTransfer(const Buffer& buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept -> BufferBarrier;

    /// Compute write + indirect draw consumption to subsequent transfer/clear reset.
    [[nodiscard]] static constexpr auto
        IndirectToTransfer(VkBuffer buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept -> BufferBarrier {
        return {
            buffer,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            offset,
            size,
        };
    }

    [[nodiscard]] static constexpr auto IndirectToTransfer(BufferSlice slice) noexcept -> BufferBarrier {
        return IndirectToTransfer(slice.buffer, slice.offset, slice.size);
    }

    [[nodiscard]] static auto IndirectToTransfer(const Buffer& buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept -> BufferBarrier;

    /// Compute culling / command generation write to indirect draw/dispatch argument read.
    [[nodiscard]] static constexpr auto
        ComputeToIndirect(VkBuffer buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept -> BufferBarrier {
        return {
            buffer,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
            offset,
            size,
        };
    }

    [[nodiscard]] static constexpr auto ComputeToIndirect(BufferSlice slice) noexcept -> BufferBarrier {
        return ComputeToIndirect(slice.buffer, slice.offset, slice.size);
    }

    [[nodiscard]] static auto ComputeToIndirect(const Buffer& buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept -> BufferBarrier;

    /// Compute shader SSBO/BDA write to subsequent compute shader SSBO/BDA read.
    [[nodiscard]] static constexpr auto
        ComputeWriteToRead(VkBuffer buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept -> BufferBarrier {
        return {
            buffer,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            offset,
            size,
        };
    }

    [[nodiscard]] static constexpr auto ComputeWriteToRead(BufferSlice slice) noexcept -> BufferBarrier {
        return ComputeWriteToRead(slice.buffer, slice.offset, slice.size);
    }

    [[nodiscard]] static auto ComputeWriteToRead(const Buffer& buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept -> BufferBarrier;

    /// Serialize overlapping staged copy writes targeting the same destination buffer range.
    [[nodiscard]] static constexpr auto
        CopyWriteToWrite(VkBuffer buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept -> BufferBarrier {
        return {
            buffer, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, offset, size,
        };
    }

    [[nodiscard]] static constexpr auto CopyWriteToWrite(BufferSlice slice) noexcept -> BufferBarrier {
        return CopyWriteToWrite(slice.buffer, slice.offset, slice.size);
    }

    [[nodiscard]] static auto CopyWriteToWrite(const Buffer& buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept -> BufferBarrier;

    /// Transfer/copy write to indirect draw/dispatch argument read.
    [[nodiscard]] static constexpr auto
        TransferToIndirect(VkBuffer buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept -> BufferBarrier {
        return {
            buffer,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
            offset,
            size,
        };
    }

    [[nodiscard]] static constexpr auto TransferToIndirect(BufferSlice slice) noexcept -> BufferBarrier {
        return TransferToIndirect(slice.buffer, slice.offset, slice.size);
    }

    [[nodiscard]] static auto TransferToIndirect(const Buffer& buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept -> BufferBarrier;

    /// Staged upload / transfer write to vertex attribute, index, and vertex shader BDA reads.
    [[nodiscard]] static constexpr auto
        TransferToVertex(VkBuffer buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept -> BufferBarrier {
        return {
            buffer,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT,
            offset,
            size,
        };
    }

    [[nodiscard]] static constexpr auto TransferToVertex(BufferSlice slice) noexcept -> BufferBarrier {
        return TransferToVertex(slice.buffer, slice.offset, slice.size);
    }

    [[nodiscard]] static auto TransferToVertex(const Buffer& buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept -> BufferBarrier;

    /// Queue-family ownership release on the source queue.
    [[nodiscard]] static constexpr auto QueueRelease(
        VkBuffer              buffer,
        VkPipelineStageFlags2 srcStage,
        VkAccessFlags2        srcAccess,
        uint32_t              srcQueueFamily,
        uint32_t              dstQueueFamily,
        VkDeviceSize          offset = 0,
        VkDeviceSize          size   = VK_WHOLE_SIZE
    ) noexcept -> BufferBarrier {
        return {
            buffer, srcStage, srcAccess, VK_PIPELINE_STAGE_2_NONE, 0, offset, size, srcQueueFamily, dstQueueFamily,
        };
    }

    [[nodiscard]] static constexpr auto
        QueueRelease(BufferSlice slice, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, uint32_t srcQueueFamily, uint32_t dstQueueFamily) noexcept
        -> BufferBarrier {
        return QueueRelease(slice.buffer, srcStage, srcAccess, srcQueueFamily, dstQueueFamily, slice.offset, slice.size);
    }

    [[nodiscard]] static auto QueueRelease(
        const Buffer&         buffer,
        VkPipelineStageFlags2 srcStage,
        VkAccessFlags2        srcAccess,
        uint32_t              srcQueueFamily,
        uint32_t              dstQueueFamily,
        VkDeviceSize          offset = 0,
        VkDeviceSize          size   = VK_WHOLE_SIZE
    ) noexcept -> BufferBarrier;

    /// Queue-family ownership acquire on the destination queue.
    [[nodiscard]] static constexpr auto QueueAcquire(
        VkBuffer              buffer,
        VkPipelineStageFlags2 dstStage,
        VkAccessFlags2        dstAccess,
        uint32_t              srcQueueFamily,
        uint32_t              dstQueueFamily,
        VkDeviceSize          offset = 0,
        VkDeviceSize          size   = VK_WHOLE_SIZE
    ) noexcept -> BufferBarrier {
        return {
            buffer, VK_PIPELINE_STAGE_2_NONE, 0, dstStage, dstAccess, offset, size, srcQueueFamily, dstQueueFamily,
        };
    }

    [[nodiscard]] static constexpr auto
        QueueAcquire(BufferSlice slice, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, uint32_t srcQueueFamily, uint32_t dstQueueFamily) noexcept
        -> BufferBarrier {
        return QueueAcquire(slice.buffer, dstStage, dstAccess, srcQueueFamily, dstQueueFamily, slice.offset, slice.size);
    }

    [[nodiscard]] static auto QueueAcquire(
        const Buffer&         buffer,
        VkPipelineStageFlags2 dstStage,
        VkAccessFlags2        dstAccess,
        uint32_t              srcQueueFamily,
        uint32_t              dstQueueFamily,
        VkDeviceSize          offset = 0,
        VkDeviceSize          size   = VK_WHOLE_SIZE
    ) noexcept -> BufferBarrier;

    // -------------------------------------------------------------------------
    // Execution ("Tell the barrier to record")
    // -------------------------------------------------------------------------

    void Record(VkCommandBuffer cmd) const noexcept {
        if (cmd == VK_NULL_HANDLE || !Valid()) {
            return;
        }
        const VkBufferMemoryBarrier2 barrier = AsVkBarrier();
        const VkDependencyInfo       dep {
            .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers    = &barrier,
        };
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    /// Zero-allocation stack submission of a fixed-size buffer barrier batch.
    template <size_t N>
    static void Record(VkCommandBuffer cmd, const std::array<BufferBarrier, N>& barriers) noexcept {
        if constexpr (N == 0) {
            return;
        } else {
            if (cmd == VK_NULL_HANDLE) {
                return;
            }
            std::array<VkBufferMemoryBarrier2, N> vkBarriers {};
            uint32_t                              count = 0;
            for (size_t i = 0; i < N; ++i) {
                if (barriers[i].Valid()) {
                    vkBarriers[count++] = barriers[i].AsVkBarrier();
                }
            }
            if (count == 0) {
                return;
            }
            const VkDependencyInfo dep {
                .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .bufferMemoryBarrierCount = count,
                .pBufferMemoryBarriers    = vkBarriers.data(),
            };
            vkCmdPipelineBarrier2(cmd, &dep);
        }
    }

    /// Dynamic-count batch submission (stack-allocated up to 16 barriers).
    static void Record(VkCommandBuffer cmd, std::span<const BufferBarrier> barriers) noexcept {
        if (barriers.empty() || cmd == VK_NULL_HANDLE) {
            return;
        }

        constexpr size_t kStackLimit = 16;
        if (barriers.size() <= kStackLimit) {
            std::array<VkBufferMemoryBarrier2, kStackLimit> vkBarriers {};
            uint32_t                                        count = 0;
            for (const auto& b: barriers) {
                if (b.Valid()) {
                    vkBarriers[count++] = b.AsVkBarrier();
                }
            }
            if (count == 0) {
                return;
            }
            const VkDependencyInfo dep {
                .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .bufferMemoryBarrierCount = count,
                .pBufferMemoryBarriers    = vkBarriers.data(),
            };
            vkCmdPipelineBarrier2(cmd, &dep);
        } else {
            std::vector<VkBufferMemoryBarrier2> heapBarriers;
            heapBarriers.reserve(barriers.size());
            for (const auto& b: barriers) {
                if (b.Valid()) {
                    heapBarriers.push_back(b.AsVkBarrier());
                }
            }
            if (heapBarriers.empty()) {
                return;
            }
            const VkDependencyInfo dep {
                .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .bufferMemoryBarrierCount = static_cast<uint32_t>(heapBarriers.size()),
                .pBufferMemoryBarriers    = heapBarriers.data(),
            };
            vkCmdPipelineBarrier2(cmd, &dep);
        }
    }

    // -------------------------------------------------------------------------
    // JIT Stack Synthesis
    // -------------------------------------------------------------------------

    [[nodiscard]] constexpr auto AsVkBarrier() const noexcept -> VkBufferMemoryBarrier2 {
        return {
            .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .pNext               = nullptr,
            .srcStageMask        = _srcStage,
            .srcAccessMask       = _srcAccess,
            .dstStageMask        = _dstStage,
            .dstAccessMask       = _dstAccess,
            .srcQueueFamilyIndex = _srcQueueFamily,
            .dstQueueFamilyIndex = _dstQueueFamily,
            .buffer              = _buffer,
            .offset              = _offset,
            .size                = _size,
        };
    }

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return _buffer != VK_NULL_HANDLE && _size != 0;
    }

    friend constexpr auto operator==(const BufferBarrier&, const BufferBarrier&) noexcept -> bool = default;

  private:
    VkBuffer              _buffer         = VK_NULL_HANDLE;
    VkDeviceSize          _offset         = 0;
    VkDeviceSize          _size           = VK_WHOLE_SIZE;
    VkPipelineStageFlags2 _srcStage       = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        _srcAccess      = 0;
    VkPipelineStageFlags2 _dstStage       = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        _dstAccess      = 0;
    uint32_t              _srcQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    uint32_t              _dstQueueFamily = VK_QUEUE_FAMILY_IGNORED;
};

static_assert(sizeof(BufferBarrier) == 64, "BufferBarrier must be exactly 64 bytes.");
static_assert(std::is_trivially_copyable_v<BufferBarrier>);

// -----------------------------------------------------------------------------
// Inline ImageView & ImageSlice Synchronization Delegations
// -----------------------------------------------------------------------------

template <VkImageLayout OldLayout, VkImageLayout NewLayout>
inline void ImageView::Transition(VkCommandBuffer cmd) const noexcept {
    ImageBarrier::Transition<OldLayout, NewLayout>(*this).Record(cmd);
}

template <VkImageLayout TargetLayout>
inline void ImageView::DiscardTo(VkCommandBuffer cmd) const noexcept {
    ImageBarrier::DiscardTo<TargetLayout>(*this).Record(cmd);
}

inline void ImageView::Transition(VkCommandBuffer cmd, VkImageLayout oldLayout, VkImageLayout newLayout) const noexcept {
    ImageBarrier::Transition(*this, oldLayout, newLayout).Record(cmd);
}

inline void ImageView::Transition(
    VkCommandBuffer       cmd,
    VkImageLayout         oldLayout,
    VkImageLayout         newLayout,
    VkPipelineStageFlags2 srcStage,
    VkAccessFlags2        srcAccess,
    VkPipelineStageFlags2 dstStage,
    VkAccessFlags2        dstAccess
) const noexcept {
    ImageBarrier::Transition(*this, oldLayout, newLayout, srcStage, srcAccess, dstStage, dstAccess).Record(cmd);
}

template <VkImageLayout OldLayout, VkImageLayout NewLayout>
inline void ImageSlice::Transition(VkCommandBuffer cmd) const noexcept {
    View().Transition<OldLayout, NewLayout>(cmd);
}

template <VkImageLayout TargetLayout>
inline void ImageSlice::DiscardTo(VkCommandBuffer cmd) const noexcept {
    View().DiscardTo<TargetLayout>(cmd);
}

inline void ImageSlice::Transition(VkCommandBuffer cmd, VkImageLayout oldLayout, VkImageLayout newLayout) const noexcept {
    View().Transition(cmd, oldLayout, newLayout);
}

inline void ImageSlice::Transition(
    VkCommandBuffer       cmd,
    VkImageLayout         oldLayout,
    VkImageLayout         newLayout,
    VkPipelineStageFlags2 srcStage,
    VkAccessFlags2        srcAccess,
    VkPipelineStageFlags2 dstStage,
    VkAccessFlags2        dstAccess
) const noexcept {
    View().Transition(cmd, oldLayout, newLayout, srcStage, srcAccess, dstStage, dstAccess);
}

// -----------------------------------------------------------------------------
// Compile-Time Synthesis Invariants
// -----------------------------------------------------------------------------
static_assert([]() consteval {
    const ImageView depthArray {
        VK_NULL_HANDLE, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, 0, ImageView::kRemainingMips, 0, 4, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
    };
    const VkImageMemoryBarrier2 vk = ImageBarrier::Transition<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(depthArray).AsVkBarrier();
    return vk.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 && vk.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
           vk.newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL && vk.srcStageMask == VK_PIPELINE_STAGE_2_NONE && vk.srcAccessMask == 0 &&
           vk.dstStageMask == (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT) &&
           vk.subresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT && vk.subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS &&
           vk.subresourceRange.layerCount == 4;
}());

static_assert([]() consteval {
    const VkImageMemoryBarrier2 present = ImageBarrier::PreparePresent(VK_NULL_HANDLE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL).AsVkBarrier();
    return present.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && present.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR &&
           present.dstStageMask == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT && present.dstAccessMask == 0 && present.subresourceRange.levelCount == 1 &&
           present.subresourceRange.layerCount == 1;
}());

static_assert([]() consteval {
    const VkMemoryBarrier2 c2r = MemoryBarrier::ComputeWriteToRead().AsVkBarrier();
    const VkMemoryBarrier2 c2i = MemoryBarrier::ComputeToIndirect().AsVkBarrier();
    return c2r.sType == VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 && c2r.srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
           c2r.srcAccessMask == VK_ACCESS_2_SHADER_WRITE_BIT && c2r.dstStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
           c2r.dstAccessMask == VK_ACCESS_2_SHADER_READ_BIT && c2i.dstStageMask == VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT &&
           c2i.dstAccessMask == VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
}());

static_assert([]() consteval {
    const BufferSlice            subSlice {VK_NULL_HANDLE, 0x1000, 256, 1024};
    const VkBufferMemoryBarrier2 t2c = BufferBarrier::TransferToCompute(subSlice).AsVkBarrier();
    const VkBufferMemoryBarrier2 i2t = BufferBarrier::IndirectToTransfer(VK_NULL_HANDLE, 64, 512).AsVkBarrier();
    const VkBufferMemoryBarrier2 rel =
        BufferBarrier::QueueRelease(subSlice, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 2, 0).AsVkBarrier();
    const VkBufferMemoryBarrier2 acq =
        BufferBarrier::QueueAcquire(subSlice, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, 2, 0).AsVkBarrier();

    return t2c.sType == VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 && t2c.pNext == nullptr && t2c.offset == 256 && t2c.size == 1024 &&
           t2c.srcStageMask == VK_PIPELINE_STAGE_2_TRANSFER_BIT && t2c.srcAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT &&
           t2c.dstStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT && t2c.dstAccessMask == (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT) &&
           t2c.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && t2c.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && i2t.offset == 64 && i2t.size == 512 &&
           i2t.srcStageMask == (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT) &&
           i2t.srcAccessMask == (VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT) &&
           i2t.dstStageMask == VK_PIPELINE_STAGE_2_TRANSFER_BIT && i2t.dstAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT &&
           rel.dstStageMask == VK_PIPELINE_STAGE_2_NONE && rel.dstAccessMask == 0 && rel.srcQueueFamilyIndex == 2 && rel.dstQueueFamilyIndex == 0 &&
           acq.srcStageMask == VK_PIPELINE_STAGE_2_NONE && acq.srcAccessMask == 0 && acq.srcQueueFamilyIndex == 2 && acq.dstQueueFamilyIndex == 0 &&
           !BufferBarrier {}.Valid();
}());

} // namespace ZHLN::Vk
