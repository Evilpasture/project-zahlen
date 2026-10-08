// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../VkError.hpp"
#include "CommandRecorder.hpp"
#include <Zahlen/Error.hpp>

namespace ZHLN::Vk {

class Context;

[[nodiscard]] std::expected<void, Vk::Error> WaitIdle(VkQueue queue) noexcept;

[[nodiscard]] std::expected<void, Vk::Error> SubmitAndWait(
    VkQueue               queue,
    ExecutableCommands    cmd,
    VkSemaphore           waitSemaphore = VK_NULL_HANDLE,
    uint64_t              waitValue     = 0,
    VkPipelineStageFlags2 waitStage     = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
) noexcept;

inline constexpr VkPipelineStageFlags2 kAsyncComputeConsumerStages =
    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

[[nodiscard]] constexpr auto MakeSemaphoreSubmitInfo(
    VkSemaphore semaphore, uint64_t value, VkPipelineStageFlags2 stage
) noexcept -> VkSemaphoreSubmitInfo {
    return {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = semaphore,
        .value = value,
        .stageMask = stage,
    };
}

[[nodiscard]] auto QueueSubmit(
    VkQueue queue,
    ExecutableCommands cmds,
    std::span<const VkSemaphoreSubmitInfo> waits = {},
    std::span<const VkSemaphoreSubmitInfo> signals = {},
    VkFence fence = VK_NULL_HANDLE
) noexcept -> std::expected<void, Vk::Error>;

[[nodiscard]] auto QueueSubmit(
    VkQueue queue,
    ExecutableCommands cmd,
    VkSemaphore waitSemaphore = VK_NULL_HANDLE,
    uint64_t waitValue = 0,
    VkPipelineStageFlags2 waitStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    VkSemaphore signalSemaphore = VK_NULL_HANDLE,
    uint64_t signalValue = 0,
    VkPipelineStageFlags2 signalStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    VkFence fence = VK_NULL_HANDLE
) noexcept -> std::expected<void, Vk::Error>;

// NOLINTNEXTLINE(performance-enum-size)
enum class BarrierStage : VkPipelineStageFlags2 {
    StageNone                  = 0,
    TopOfPipe                  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
    BottomOfPipe               = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
    Compute                    = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
    Fragment                   = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
    Vertex                     = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
    ColorAttachment            = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    EarlyFragment              = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
    LateFragment               = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
    Indirect                   = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
    Transfer                   = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
    Copy                       = VK_PIPELINE_STAGE_2_COPY_BIT,
    Clear                      = VK_PIPELINE_STAGE_2_CLEAR_BIT,
    Host                       = VK_PIPELINE_STAGE_2_HOST_BIT,
    AccelerationStructureBuild = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
};

// NOLINTNEXTLINE(performance-enum-size)
enum class BarrierAccess : VkAccessFlags2 {
    AccessNone    = 0,
    ShaderRead    = VK_ACCESS_2_SHADER_READ_BIT,
    ShaderWrite   = VK_ACCESS_2_SHADER_WRITE_BIT,
    IndirectRead  = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
    TransferRead  = VK_ACCESS_2_TRANSFER_READ_BIT,
    TransferWrite = VK_ACCESS_2_TRANSFER_WRITE_BIT,
    HostRead      = VK_ACCESS_2_HOST_READ_BIT,
    HostWrite     = VK_ACCESS_2_HOST_WRITE_BIT,
    ColorRead     = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
    ColorWrite    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
    DepthRead     = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
    DepthWrite    = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,

    AccelerationStructureRead  = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
    AccelerationStructureWrite = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR
};

[[nodiscard]] constexpr auto operator|(BarrierStage a, BarrierStage b) noexcept -> BarrierStage;
[[nodiscard]] constexpr auto operator|(BarrierAccess a, BarrierAccess b) noexcept -> BarrierAccess;

inline void PipelineBarrier(
    VkCommandBuffer cmd,
    std::span<const VkBufferMemoryBarrier2> buffers = {},
    std::span<const VkImageMemoryBarrier2>  images  = {},
    std::span<const VkMemoryBarrier2>       memory  = {}
) noexcept;

struct MemoryBarrierDesc {
    VkPipelineStageFlags2 src_stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        src_access = VK_ACCESS_2_NONE;
    VkPipelineStageFlags2 dst_stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        dst_access = VK_ACCESS_2_NONE;
};

struct ImageBarrierDesc {
    VkImage               image = VK_NULL_HANDLE;
    VkAccessFlags2        src_access = VK_ACCESS_2_NONE;
    VkAccessFlags2        dst_access = VK_ACCESS_2_NONE;
    VkImageLayout         src_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout         dst_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 src_stage = VK_PIPELINE_STAGE_2_NONE;
    VkPipelineStageFlags2 dst_stage = VK_PIPELINE_STAGE_2_NONE;
    VkImageAspectFlags    aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t              base_mip = 0;
    uint32_t              mip_count = VK_REMAINING_MIP_LEVELS;
    uint32_t              base_array_layer = 0;
    uint32_t              layer_count = VK_REMAINING_ARRAY_LAYERS;
};

[[nodiscard]] constexpr auto MakeMemoryBarrier(const MemoryBarrierDesc& desc) noexcept -> VkMemoryBarrier2 {
    return {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = desc.src_stage,
        .srcAccessMask = desc.src_access,
        .dstStageMask  = desc.dst_stage,
        .dstAccessMask = desc.dst_access,
    };
}

[[nodiscard]] constexpr auto MakeImageBarrier(const ImageBarrierDesc& desc) noexcept -> VkImageMemoryBarrier2 {
    return {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = desc.src_stage,
        .srcAccessMask       = desc.src_access,
        .dstStageMask        = desc.dst_stage,
        .dstAccessMask       = desc.dst_access,
        .oldLayout           = desc.src_layout,
        .newLayout           = desc.dst_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = desc.image,
        .subresourceRange    = {
            .aspectMask     = desc.aspect,
            .baseMipLevel   = desc.base_mip,
            .levelCount     = desc.mip_count,
            .baseArrayLayer = desc.base_array_layer,
            .layerCount     = desc.layer_count,
        },
    };
}

inline void MemoryBarrier(VkCommandBuffer cmd, const MemoryBarrierDesc& desc) noexcept;
inline void ImageBarrier(VkCommandBuffer cmd, const ImageBarrierDesc& desc) noexcept;

inline void MemoryBarrier(
    VkCommandBuffer cmd,
    BarrierStage    srcStage,
    BarrierAccess   srcAccess,
    BarrierStage    dstStage,
    BarrierAccess   dstAccess
) noexcept;

enum class QueueType : uint8_t { Graphics, Compute, Transfer };

template <QueueType QType>
struct CommandBuffer {
    VkCommandBuffer            handle     = VK_NULL_HANDLE;
    static constexpr QueueType queue_type = QType;

                       operator VkCommandBuffer() const noexcept;
    [[nodiscard]] bool Valid() const noexcept;
};

[[nodiscard]] constexpr auto MakeBufferBarrier(
    VkBuffer      buffer,
    BarrierStage  srcStage,
    BarrierAccess srcAccess,
    BarrierStage  dstStage,
    BarrierAccess dstAccess
) noexcept -> VkBufferMemoryBarrier2 {
    return {
        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .pNext               = nullptr,
        .srcStageMask        = static_cast<VkPipelineStageFlags2>(srcStage),
        .srcAccessMask       = static_cast<VkAccessFlags2>(srcAccess),
        .dstStageMask        = static_cast<VkPipelineStageFlags2>(dstStage),
        .dstAccessMask       = static_cast<VkAccessFlags2>(dstAccess),
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer              = buffer,
        .offset              = 0,
        .size                = VK_WHOLE_SIZE
    };
}

inline void BufferBarrier(
    VkCommandBuffer cmd,
    VkBuffer        buffer,
    BarrierStage    srcStage,
    BarrierAccess   srcAccess,
    BarrierStage    dstStage,
    BarrierAccess   dstAccess
) noexcept {
    const VkBufferMemoryBarrier2 barrier = MakeBufferBarrier(buffer, srcStage, srcAccess, dstStage, dstAccess);
    PipelineBarrier(cmd, std::span<const VkBufferMemoryBarrier2>(&barrier, 1));
}

template <QueueType QType>
[[nodiscard]] constexpr auto ResolveQueue(const Context& ctx) noexcept -> VkQueue;

template <QueueType QType>
[[nodiscard]] constexpr auto ResolveQueueFamily(const Context& ctx) noexcept -> uint32_t;

template <QueueType QType>
[[nodiscard]] inline auto QueueSubmit(
    const Context& ctx,
    ExecutableCommands cmd,
    VkSemaphore waitSemaphore = VK_NULL_HANDLE,
    uint64_t waitValue = 0,
    VkPipelineStageFlags2 waitStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    VkSemaphore signalSemaphore = VK_NULL_HANDLE,
    uint64_t signalValue = 0,
    VkPipelineStageFlags2 signalStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    VkFence fence = VK_NULL_HANDLE
) noexcept -> std::expected<void, Vk::Error> {
    return QueueSubmit(
        ResolveQueue<QType>(ctx), std::move(cmd), waitSemaphore, waitValue, waitStage, signalSemaphore, signalValue, signalStage, fence
    );
}

}

#include "RenderQueue.inl"
