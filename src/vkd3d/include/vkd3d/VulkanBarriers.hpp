// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Vulkan synthesis and execution for the VkD3D TDA barrier layer
// (<vkd3d/Barriers.hpp>), the Vulkan-facing half of the joint layer
// alongside <vkd3d/VulkanConversions.hpp>: AsVkBarrier() lowers a
// VkD3D::{Memory,Buffer,Image}Barrier to the Vk*MemoryBarrier2 C struct
// on the stack, and the Record() members declared in <vkd3d/Barriers.hpp>
// are defined here for VkCommandBuffer (one vkCmdPipelineBarrier2 call
// per batch, never one per barrier). Nothing under src/vulkan names a
// VkD3D type -- the backend keeps its own native barrier classes
// (src/vulkan/execution/Barriers.hpp); this header is how the joint
// layer and the renderer tell VkD3D barriers to record on Vulkan.

#include <vkd3d/Barriers.hpp>
#include <vkd3d/VulkanConversions.hpp>
#include <vulkan/Enums.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace ZHLN::VkD3D {

// VkImage/VkBuffer are 64-bit non-dispatchable handles (pointer-typed
// under VK_USE_64_BIT_PTR_DEFINES); the neutral values store exactly that
// word. Pointer bit-casts are not constant expressions, so in consteval
// contexts -- where an opaque handle has no numeric value anyway -- only
// null identity is preserved; runtime values are always the exact bits.
[[nodiscard]] constexpr auto ImageFromWord(std::uint64_t word) noexcept -> VkImage {
    if consteval {
        return VK_NULL_HANDLE;
    }
    return std::bit_cast<VkImage>(word);
}

[[nodiscard]] constexpr auto BufferFromWord(std::uint64_t word) noexcept -> VkBuffer {
    if consteval {
        return VK_NULL_HANDLE;
    }
    return std::bit_cast<VkBuffer>(word);
}

[[nodiscard]] constexpr auto WordFromImage(VkImage image) noexcept -> std::uint64_t {
    if consteval {
        return image == VK_NULL_HANDLE ? std::uint64_t {0} : ~std::uint64_t {0};
    }
    return std::bit_cast<std::uint64_t>(image);
}

[[nodiscard]] constexpr auto WordFromBuffer(VkBuffer buffer) noexcept -> std::uint64_t {
    if consteval {
        return buffer == VK_NULL_HANDLE ? std::uint64_t {0} : ~std::uint64_t {0};
    }
    return std::bit_cast<std::uint64_t>(buffer);
}

// -----------------------------------------------------------------------------
// JIT stack synthesis
// -----------------------------------------------------------------------------

[[nodiscard]] constexpr auto AsVkBarrier(const MemoryBarrier& barrier) noexcept -> VkMemoryBarrier2 {
    return {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext         = nullptr,
        .srcStageMask  = Vk::ToVk(ToVk(barrier.stageBefore)),
        .srcAccessMask = Vk::ToVk(ToVk(barrier.accessBefore)),
        .dstStageMask  = Vk::ToVk(ToVk(barrier.stageAfter)),
        .dstAccessMask = Vk::ToVk(ToVk(barrier.accessAfter)),
    };
}

[[nodiscard]] constexpr auto AsVkBarrier(const BufferBarrier& barrier) noexcept -> VkBufferMemoryBarrier2 {
    return {
        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .pNext               = nullptr,
        .srcStageMask        = Vk::ToVk(ToVk(barrier.stageBefore)),
        .srcAccessMask       = Vk::ToVk(ToVk(barrier.accessBefore)),
        .dstStageMask        = Vk::ToVk(ToVk(barrier.stageAfter)),
        .dstAccessMask       = Vk::ToVk(ToVk(barrier.accessAfter)),
        .srcQueueFamilyIndex = barrier.srcQueueFamily,
        .dstQueueFamilyIndex = barrier.dstQueueFamily,
        .buffer              = BufferFromWord(barrier.resource),
        .offset              = barrier.offset,
        .size                = barrier.size,
    };
}

[[nodiscard]] constexpr auto AsVkBarrier(const ImageBarrier& barrier) noexcept -> VkImageMemoryBarrier2 {
    return {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext               = nullptr,
        .srcStageMask        = Vk::ToVk(ToVk(barrier.stageBefore)),
        .srcAccessMask       = Vk::ToVk(ToVk(barrier.accessBefore)),
        .dstStageMask        = Vk::ToVk(ToVk(barrier.stageAfter)),
        .dstAccessMask       = Vk::ToVk(ToVk(barrier.accessAfter)),
        .oldLayout           = Vk::ToVk(ToVk(barrier.layoutBefore)),
        .newLayout           = Vk::ToVk(ToVk(barrier.layoutAfter)),
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = ImageFromWord(barrier.resource),
        .subresourceRange    = {
            .aspectMask     = Vk::ToVk(ToVk(barrier.aspect)),
            .baseMipLevel   = barrier.baseMipLevel,
            .levelCount     = barrier.mipLevelCount == ImageBarrier::kRemainingMips ? VK_REMAINING_MIP_LEVELS : barrier.mipLevelCount,
            .baseArrayLayer = barrier.baseArrayLayer,
            .layerCount     = barrier.arrayLayerCount,
        },
    };
}

namespace TemplatedDetail {

// -----------------------------------------------------------------------------
// Recording
// -----------------------------------------------------------------------------

inline void RecordBarrier(VkCommandBuffer cmd, const MemoryBarrier& barrier) noexcept {
    if (cmd == VK_NULL_HANDLE) {
        return;
    }
    const VkMemoryBarrier2 mem = AsVkBarrier(barrier);
    const VkDependencyInfo dep {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &mem,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

template <size_t N>
void RecordBarrier(VkCommandBuffer cmd, const std::array<MemoryBarrier, N>& barriers) noexcept {
    if constexpr (N == 0) {
        return;
    } else {
        if (cmd == VK_NULL_HANDLE) {
            return;
        }
        std::array<VkMemoryBarrier2, N> vkBarriers {};
        for (size_t i = 0; i < N; ++i) {
            vkBarriers[i] = AsVkBarrier(barriers[i]);
        }
        const VkDependencyInfo dep {
            .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = static_cast<uint32_t>(N),
            .pMemoryBarriers    = vkBarriers.data(),
        };
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}

inline void RecordBarrier(VkCommandBuffer cmd, std::span<const MemoryBarrier> barriers) noexcept {
    if (barriers.empty() || cmd == VK_NULL_HANDLE) {
        return;
    }
    constexpr size_t kStackLimit = 8;
    if (barriers.size() <= kStackLimit) {
        std::array<VkMemoryBarrier2, kStackLimit> vkBarriers {};
        for (size_t i = 0; i < barriers.size(); ++i) {
            vkBarriers[i] = AsVkBarrier(barriers[i]);
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
            heapBarriers.push_back(AsVkBarrier(b));
        }
        const VkDependencyInfo dep {
            .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = static_cast<uint32_t>(heapBarriers.size()),
            .pMemoryBarriers    = heapBarriers.data(),
        };
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}

inline void RecordBarrier(VkCommandBuffer cmd, const BufferBarrier& barrier) noexcept {
    if (cmd == VK_NULL_HANDLE || !barrier.Valid()) {
        return;
    }
    const VkBufferMemoryBarrier2 vkBarrier = AsVkBarrier(barrier);
    const VkDependencyInfo       dep {
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers    = &vkBarrier,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

template <size_t N>
void RecordBarrier(VkCommandBuffer cmd, const std::array<BufferBarrier, N>& barriers) noexcept {
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
                vkBarriers[count++] = AsVkBarrier(barriers[i]);
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

inline void RecordBarrier(VkCommandBuffer cmd, std::span<const BufferBarrier> barriers) noexcept {
    if (barriers.empty() || cmd == VK_NULL_HANDLE) {
        return;
    }
    constexpr size_t kStackLimit = 16;
    if (barriers.size() <= kStackLimit) {
        std::array<VkBufferMemoryBarrier2, kStackLimit> vkBarriers {};
        uint32_t                                        count = 0;
        for (const auto& b: barriers) {
            if (b.Valid()) {
                vkBarriers[count++] = AsVkBarrier(b);
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
                heapBarriers.push_back(AsVkBarrier(b));
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

inline void RecordBarrier(VkCommandBuffer cmd, const ImageBarrier& barrier) noexcept {
    if (cmd == VK_NULL_HANDLE || !barrier.Valid()) {
        return;
    }
    const VkImageMemoryBarrier2 vkBarrier = AsVkBarrier(barrier);
    const VkDependencyInfo      dep {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &vkBarrier,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

template <size_t N>
void RecordBarrier(VkCommandBuffer cmd, const std::array<ImageBarrier, N>& barriers) noexcept {
    if constexpr (N == 0) {
        return;
    } else {
        if (cmd == VK_NULL_HANDLE) {
            return;
        }
        std::array<VkImageMemoryBarrier2, N> vkBarriers {};
        for (size_t i = 0; i < N; ++i) {
            vkBarriers[i] = AsVkBarrier(barriers[i]);
        }
        const VkDependencyInfo dep {
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = static_cast<uint32_t>(N),
            .pImageMemoryBarriers    = vkBarriers.data(),
        };
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}

inline void RecordBarrier(VkCommandBuffer cmd, std::span<const ImageBarrier> barriers) noexcept {
    if (barriers.empty() || cmd == VK_NULL_HANDLE) {
        return;
    }
    constexpr size_t kStackLimit = 16;
    if (barriers.size() <= kStackLimit) {
        std::array<VkImageMemoryBarrier2, kStackLimit> vkBarriers {};
        for (size_t i = 0; i < barriers.size(); ++i) {
            vkBarriers[i] = AsVkBarrier(barriers[i]);
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
            heapBarriers.push_back(AsVkBarrier(b));
        }
        const VkDependencyInfo dep {
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = static_cast<uint32_t>(heapBarriers.size()),
            .pImageMemoryBarriers    = heapBarriers.data(),
        };
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}

} // namespace TemplatedDetail

// -----------------------------------------------------------------------------
// Record() member definitions (declared in <vkd3d/Barriers.hpp>): telling a
// barrier to record is the TDA verb; this header supplies the Vulkan sink.
// -----------------------------------------------------------------------------

template <typename CommandBuffer>
void MemoryBarrier::Record(CommandBuffer cmd) const noexcept {
    static_assert(std::is_same_v<CommandBuffer, VkCommandBuffer>, "VkD3D::MemoryBarrier::Record is defined for VkCommandBuffer by <vkd3d/VulkanBarriers.hpp>");
    TemplatedDetail::RecordBarrier(cmd, *this);
}

template <typename CommandBuffer, std::size_t N>
void MemoryBarrier::Record(CommandBuffer cmd, const std::array<MemoryBarrier, N>& barriers) noexcept {
    static_assert(std::is_same_v<CommandBuffer, VkCommandBuffer>, "VkD3D::MemoryBarrier::Record is defined for VkCommandBuffer by <vkd3d/VulkanBarriers.hpp>");
    TemplatedDetail::RecordBarrier(cmd, barriers);
}

template <typename CommandBuffer>
void MemoryBarrier::Record(CommandBuffer cmd, std::span<const MemoryBarrier> barriers) noexcept {
    static_assert(std::is_same_v<CommandBuffer, VkCommandBuffer>, "VkD3D::MemoryBarrier::Record is defined for VkCommandBuffer by <vkd3d/VulkanBarriers.hpp>");
    TemplatedDetail::RecordBarrier(cmd, barriers);
}

template <typename CommandBuffer>
void BufferBarrier::Record(CommandBuffer cmd) const noexcept {
    static_assert(std::is_same_v<CommandBuffer, VkCommandBuffer>, "VkD3D::BufferBarrier::Record is defined for VkCommandBuffer by <vkd3d/VulkanBarriers.hpp>");
    TemplatedDetail::RecordBarrier(cmd, *this);
}

template <typename CommandBuffer, std::size_t N>
void BufferBarrier::Record(CommandBuffer cmd, const std::array<BufferBarrier, N>& barriers) noexcept {
    static_assert(std::is_same_v<CommandBuffer, VkCommandBuffer>, "VkD3D::BufferBarrier::Record is defined for VkCommandBuffer by <vkd3d/VulkanBarriers.hpp>");
    TemplatedDetail::RecordBarrier(cmd, barriers);
}

template <typename CommandBuffer>
void BufferBarrier::Record(CommandBuffer cmd, std::span<const BufferBarrier> barriers) noexcept {
    static_assert(std::is_same_v<CommandBuffer, VkCommandBuffer>, "VkD3D::BufferBarrier::Record is defined for VkCommandBuffer by <vkd3d/VulkanBarriers.hpp>");
    TemplatedDetail::RecordBarrier(cmd, barriers);
}

template <typename CommandBuffer>
void ImageBarrier::Record(CommandBuffer cmd) const noexcept {
    static_assert(std::is_same_v<CommandBuffer, VkCommandBuffer>, "VkD3D::ImageBarrier::Record is defined for VkCommandBuffer by <vkd3d/VulkanBarriers.hpp>");
    TemplatedDetail::RecordBarrier(cmd, *this);
}

template <typename CommandBuffer, std::size_t N>
void ImageBarrier::Record(CommandBuffer cmd, const std::array<ImageBarrier, N>& barriers) noexcept {
    static_assert(std::is_same_v<CommandBuffer, VkCommandBuffer>, "VkD3D::ImageBarrier::Record is defined for VkCommandBuffer by <vkd3d/VulkanBarriers.hpp>");
    TemplatedDetail::RecordBarrier(cmd, barriers);
}

template <typename CommandBuffer>
void ImageBarrier::Record(CommandBuffer cmd, std::span<const ImageBarrier> barriers) noexcept {
    static_assert(std::is_same_v<CommandBuffer, VkCommandBuffer>, "VkD3D::ImageBarrier::Record is defined for VkCommandBuffer by <vkd3d/VulkanBarriers.hpp>");
    TemplatedDetail::RecordBarrier(cmd, barriers);
}

} // namespace ZHLN::VkD3D
