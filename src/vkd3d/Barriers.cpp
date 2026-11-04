// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vkd3d/Barriers.hpp>

#include <vkd3d/VulkanBarriers.hpp>

// Anchor TU for the neutral barrier values. The headers are fully constexpr;
// these checks pin the factory vocabulary (stages, accesses, sentinels) so a
// drifted enumerator or a reordered field fails here instead of at a backend.
// Vulkan synthesis invariants are at the bottom of this file; the D3D12
// direction is pinned in D3D12Conversions.cpp.

namespace {

using namespace ZHLN::VkD3D;

static_assert([]() consteval {
    const MemoryBarrier c2r = MemoryBarrier::ComputeWriteToRead();
    const MemoryBarrier c2i = MemoryBarrier::ComputeToIndirect();
    return c2r.stageBefore == PipelineStage::Compute && c2r.accessBefore == Access::ShaderWrite && c2r.stageAfter == PipelineStage::Compute &&
           c2r.accessAfter == Access::ShaderRead && c2i.stageAfter == PipelineStage::Indirect && c2i.accessAfter == Access::IndirectRead &&
           MemoryBarrier {}.stageBefore == PipelineStage::None;
}());

static_assert([]() consteval {
    const BufferBarrier t2c = BufferBarrier::TransferToCompute(0x1000, 256, 1024);
    const BufferBarrier rel = BufferBarrier::QueueRelease(0x1000, PipelineStage::Transfer, Access::TransferWrite, 2, 0);
    const BufferBarrier acq = BufferBarrier::QueueAcquire(0x1000, PipelineStage::Compute, Access::ShaderRead, 2, 0);
    return t2c.resource == 0x1000 && t2c.offset == 256 && t2c.size == 1024 && t2c.stageBefore == PipelineStage::Transfer &&
           t2c.accessAfter == (Access::ShaderRead | Access::ShaderWrite) && t2c.srcQueueFamily == BufferBarrier::kQueueFamilyIgnored &&
           rel.stageAfter == PipelineStage::None && rel.accessAfter == Access::None && rel.srcQueueFamily == 2 && rel.dstQueueFamily == 0 &&
           acq.stageBefore == PipelineStage::None && acq.srcQueueFamily == 2 && acq.Valid() && !BufferBarrier {}.Valid() &&
           BufferBarrier::TransferToCompute(0x1000).size == BufferBarrier::kWholeSize;
}());

static_assert([]() consteval {
    const ImageBarrier depth = ImageBarrier::Transition<ImageLayout::Undefined, ImageLayout::DepthAttachmentOptimal>(
        0x2000, {.aspect = ImageAspect::Depth, .baseMipLevel = 0, .mipLevelCount = ImageBarrier::kRemainingMips, .baseArrayLayer = 0, .arrayLayerCount = 4}
    );
    const ImageBarrier present = ImageBarrier::PreparePresent(0x2000, ImageLayout::ColorAttachmentOptimal);
    return depth.layoutBefore == ImageLayout::Undefined && depth.layoutAfter == ImageLayout::DepthAttachmentOptimal &&
           depth.stageBefore == PipelineStage::None && depth.stageAfter == (PipelineStage::EarlyFragment | PipelineStage::LateFragment) &&
           depth.aspect == ImageAspect::Depth && depth.mipLevelCount == ImageBarrier::kRemainingMips && depth.arrayLayerCount == 4 &&
           present.layoutAfter == ImageLayout::PresentSrc && present.stageAfter == PipelineStage::BottomOfPipe &&
           present.accessAfter == Access::None && present.mipLevelCount == 1 && present.arrayLayerCount == 1;
}());

static_assert([]() consteval {
    // Runtime (graph-style) transitions derive the same masks as the traits.
    const ImageBarrier rt = ImageBarrier::Transition(0x2000, {}, ImageLayout::TransferDstOptimal, ImageLayout::ShaderReadOnlyOptimal);
    return rt.stageBefore == PipelineStage::Transfer && rt.accessBefore == Access::TransferWrite &&
           rt.accessAfter == Access::ShaderRead && LayoutTraits<ImageLayout::General>::kStage == PipelineStage::Compute &&
           LayoutTraits<ImageLayout::ShaderReadOnlyOptimal>::kStage == PipelineStage::Fragment;
}());

// Vulkan synthesis: the neutral values lower to exactly the native masks
// the old native-storage barrier classes produced (these mirror the
// invariants that used to live in src/vulkan/execution/Barriers.hpp).
static_assert([]() consteval {
    const VkImageMemoryBarrier2 vk = AsVkBarrier(ImageBarrier::Transition<ImageLayout::Undefined, ImageLayout::DepthAttachmentOptimal>(
        0, {.aspect = ImageAspect::Depth, .baseMipLevel = 0, .mipLevelCount = ImageBarrier::kRemainingMips, .baseArrayLayer = 0, .arrayLayerCount = 4}
    ));
    return vk.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 && vk.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
           vk.newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL && vk.srcStageMask == VK_PIPELINE_STAGE_2_NONE && vk.srcAccessMask == 0 &&
           vk.dstStageMask == (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT) &&
           vk.subresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT && vk.subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS &&
           vk.subresourceRange.layerCount == 4;
}());

static_assert([]() consteval {
    const VkImageMemoryBarrier2 present = AsVkBarrier(ImageBarrier::PreparePresent(0, ImageLayout::ColorAttachmentOptimal));
    return present.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && present.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR &&
           present.dstStageMask == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT && present.dstAccessMask == 0 && present.subresourceRange.levelCount == 1 &&
           present.subresourceRange.layerCount == 1;
}());

static_assert([]() consteval {
    const VkMemoryBarrier2 c2r = AsVkBarrier(MemoryBarrier::ComputeWriteToRead());
    const VkMemoryBarrier2 c2i = AsVkBarrier(MemoryBarrier::ComputeToIndirect());
    return c2r.sType == VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 && c2r.srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
           c2r.srcAccessMask == VK_ACCESS_2_SHADER_WRITE_BIT && c2r.dstStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
           c2r.dstAccessMask == VK_ACCESS_2_SHADER_READ_BIT && c2i.dstStageMask == VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT &&
           c2i.dstAccessMask == VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
}());

static_assert([]() consteval {
    const VkBufferMemoryBarrier2 t2c = AsVkBarrier(BufferBarrier::TransferToCompute(0, 256, 1024));
    const VkBufferMemoryBarrier2 rel = AsVkBarrier(BufferBarrier::QueueRelease(0, PipelineStage::Transfer, Access::TransferWrite, 2, 0));
    const VkBufferMemoryBarrier2 acq = AsVkBarrier(BufferBarrier::QueueAcquire(0, PipelineStage::Compute, Access::ShaderRead, 2, 0));
    return t2c.sType == VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 && t2c.pNext == nullptr && t2c.offset == 256 && t2c.size == 1024 &&
           t2c.srcStageMask == VK_PIPELINE_STAGE_2_TRANSFER_BIT && t2c.srcAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT &&
           t2c.dstStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
           t2c.dstAccessMask == (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT) &&
           t2c.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && rel.dstStageMask == VK_PIPELINE_STAGE_2_NONE && rel.dstAccessMask == 0 &&
           rel.srcQueueFamilyIndex == 2 && rel.dstQueueFamilyIndex == 0 && acq.srcStageMask == VK_PIPELINE_STAGE_2_NONE && acq.srcQueueFamilyIndex == 2;
}());

// The TDA factories the renderer calls: a domain object (here a minimal
// view double with Handle() + SubresourceRange()) is handed over whole;
// the barrier asks it for identity and extent.
static_assert([]() consteval {
    struct DepthView {
        [[nodiscard]] constexpr auto Handle() const noexcept -> VkImage { return VK_NULL_HANDLE; }
        [[nodiscard]] constexpr auto SubresourceRange() const noexcept -> VkImageSubresourceRange {
            return {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = VK_REMAINING_MIP_LEVELS, .baseArrayLayer = 0, .layerCount = 4,
            };
        }
    };
    const VkImageMemoryBarrier2 viaTda =
        AsVkBarrier(ImageBarrier::Transition<ImageLayout::Undefined, ImageLayout::DepthAttachmentOptimal>(DepthView {}));
    const VkImageMemoryBarrier2 discard = AsVkBarrier(ImageBarrier::DiscardTo<ImageLayout::General>(DepthView {}));
    return viaTda.newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL && viaTda.subresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT &&
           viaTda.subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS && viaTda.subresourceRange.layerCount == 4 &&
           discard.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && discard.newLayout == VK_IMAGE_LAYOUT_GENERAL &&
           discard.dstStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
}());

} // namespace
