// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "RenderCore.hpp"
#include "Utils.hpp"

namespace ZHLN::Vk {

// ============================================================================
// Command & Rendering Helpers Implementation
// ============================================================================

inline ScopedScissor::ScopedScissor(VkCommandBuffer cmd, const ScissorDesc& desc) noexcept: commandRect(cmd), resetScissor(desc.fallback) {
    vkCmdSetScissor(commandRect, 0, 1, &desc.target);
}

inline ScopedScissor::~ScopedScissor() noexcept {
    vkCmdSetScissor(commandRect, 0, 1, &resetScissor);
}

inline ScopedRendering::ScopedRendering(const VkCommandBuffer cmd, const ZHLN_RenderPassDesc& desc) noexcept: _cmd(cmd) {
    ZHLN_BeginRendering(_cmd, &desc);
}

inline ScopedRendering::~ScopedRendering() noexcept {
    ZHLN_EndRendering(_cmd);
}

inline void ImageBarrier(const VkCommandBuffer cmd, const ZHLN_ImageBarrierDesc& desc) noexcept {
    ZHLN_CmdImageBarrier(cmd, &desc);
}

inline void CopyBufferToImage(const VkCommandBuffer cmd, const ZHLN_BufferImageCopyDesc& desc) noexcept {
    ZHLN_CmdCopyBufferToImage(cmd, &desc);
}

inline void
    CopyImageToBuffer(VkCommandBuffer cmd, VkImage srcImage, VkBuffer dstBuffer, VkExtent2D extent, VkImageLayout layout, VkImageAspectFlags aspect) noexcept {
    const VkBufferImageCopy2 region = {
        .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .pNext             = nullptr,
        .bufferOffset      = 0,
        .bufferRowLength   = extent.width,
        .bufferImageHeight = extent.height,
        .imageSubresource  = {.aspectMask = aspect, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
        .imageOffset       = {0, 0, 0},
        .imageExtent       = {.width = extent.width, .height = extent.height, .depth = 1},
    };

    const VkCopyImageToBufferInfo2 copyInfo = {
        .sType          = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
        .pNext          = nullptr,
        .srcImage       = srcImage,
        .srcImageLayout = layout,
        .dstBuffer      = dstBuffer,
        .regionCount    = 1,
        .pRegions       = &region,
    };

    vkCmdCopyImageToBuffer2(cmd, &copyInfo);
}

template <size_t RegionCount>
constexpr auto CreateCopyRegions(
    VkDeviceSize       baseOffset,
    VkDeviceSize       regionSize,
    VkExtent3D         extent,
    VkImageAspectFlags aspect,
    uint32_t           mipLevel,
    uint32_t           baseArrayLayer
) noexcept -> std::array<VkBufferImageCopy2, RegionCount> {
    std::array<VkBufferImageCopy2, RegionCount> regions {};
    for (uint32_t i = 0; i < RegionCount; ++i) {
        regions[i] = {
            .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .pNext             = nullptr,
            .bufferOffset      = baseOffset + (i * regionSize),
            .bufferRowLength   = 0,
            .bufferImageHeight = 0,
            .imageSubresource  = {.aspectMask = aspect, .mipLevel = mipLevel, .baseArrayLayer = baseArrayLayer + i, .layerCount = 1},
            .imageOffset       = {0, 0, 0},
            .imageExtent       = extent
        };
    }
    return regions;
}

template <size_t RegionCount>
inline void CopyBufferToImage(
    VkCommandBuffer                                    cmd,
    VkBuffer                                           srcBuffer,
    VkImage                                            dstImage,
    const std::array<VkBufferImageCopy2, RegionCount>& regions,
    VkImageLayout                                      layout
) noexcept {
    VkCopyBufferToImageInfo2 copy_info = {
        .sType          = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .pNext          = nullptr,
        .srcBuffer      = srcBuffer,
        .dstImage       = dstImage,
        .dstImageLayout = layout,
        .regionCount    = static_cast<uint32_t>(RegionCount),
        .pRegions       = regions.data()
    };
    vkCmdCopyBufferToImage2(cmd, &copy_info);
}

template <GpuTriviallyCopyable T>
inline void Push(const VkCommandBuffer cmd, const VkPipelineLayout layout, const VkShaderStageFlags stages, const T& value) noexcept {
    ZHLN_PushConstants(cmd, layout, stages, &value, sizeof(T));
}

// ============================================================================
// VK_EXT_descriptor_heap: Push Data (replaces push constants for heap pipelines)
// ============================================================================
//
// Legacy push-constant blocks in SPIR-V read the push-data blob starting at
// offset 0, so per-draw structs are pushed at offset 0. The engine reserves
// higher offsets (e.g. RenderContext::Impl::kHeapFrameAddrPushOffset) for
// per-frame data consumed by VkDescriptorSetAndBindingMappingEXT sources such
// as VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT.

template <GpuTriviallyCopyable T>
inline void PushData(const Context& ctx, const VkCommandBuffer cmd, const uint32_t offset, const T& value) noexcept {
    static_assert(sizeof(T) % 4 == 0, "Push data size must be a multiple of 4 bytes");
    const VkPushDataInfoEXT info = {
        .sType  = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
        .pNext  = nullptr,
        .offset = offset,
        .data   = {.address = &value, .size = sizeof(T)},
    };
    ctx.CmdPushData(cmd, &info);
}

inline void PushData(const Context& ctx, const VkCommandBuffer cmd, const uint32_t offset, const void* data, const uint32_t size) noexcept {
    const VkPushDataInfoEXT info = {
        .sType  = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
        .pNext  = nullptr,
        .offset = offset,
        .data   = {.address = data, .size = size},
    };
    ctx.CmdPushData(cmd, &info);
}

template <uint32_t N, bool WaitOnFence, typename Record, typename Rebuild>
    requires RecordFn<Record> && RebuildFn<Rebuild>
inline auto DrawFrame(const DrawFrameDesc<N>& desc, uint32_t& frameIndex, Record&& record, Rebuild&& rebuild) noexcept -> ZHLN_FrameResult {
    const ZHLN_FrameSync&   s    = desc.sync[frameIndex];
    const ZHLN_CommandPool& pool = desc.pools[frameIndex];
    const VkCommandBuffer   cmd  = desc.pools.Cmd(frameIndex);

    uint32_t         image_index = 0;
    ZHLN_FrameResult result      = ZHLN_FrameResult_Ok;

    if constexpr (WaitOnFence) {
        result = ZHLN_WaitAndAcquireImage(desc.ctx.Device(), desc.swapchain.Get().handle, &s, &pool, &image_index);
    } else {
        vkResetFences(desc.ctx.Device(), 1, &s.in_flight);
        ZHLN_ResetCommandPool(desc.ctx.Device(), &pool);

        ZHLN_AcquireDesc acquire_desc = {
            .swapchain       = desc.swapchain.Get().handle,
            .image_available = s.image_available,
            .timeout_ns      = UINT64_MAX,
        };
        result = ZHLN_AcquireImage(desc.ctx.Device(), &acquire_desc, &image_index);
    }

    if (result == ZHLN_FrameResult_OutOfDate) [[unlikely]] {
        std::invoke(std::forward<Rebuild>(rebuild));
        return result;
    }

    ZHLN_BeginCommandBuffer(cmd);
    std::invoke(std::forward<Record>(record), cmd, image_index);
    ZHLN_EndCommandBuffer(cmd);

    const ZHLN_FrameSubmitDesc submit_desc = {
        .graphicsQueue    = desc.ctx.GraphicsQueue(),
        .presentQueue     = desc.ctx.PresentQueue(),
        .cmd              = cmd,
        .imageAvailable   = s.image_available,
        .renderFinished   = desc.presentSemaphores[image_index],
        .inFlight         = s.in_flight,
        .swapchain        = desc.swapchain.Get().handle,
        .imageIndex       = image_index,
        .stagingSemaphore = desc.stagingSemaphore,
        .computeSemaphore = s.compute_timeline,
        .stagingWaitValue = desc.stagingWaitValue,
        .computeWaitValue = desc.sync.GetTimelineValue(frameIndex),
    };

    result = ZHLN_SubmitAndPresent(&submit_desc);
    if (result == ZHLN_FrameResult_OutOfDate || result == ZHLN_FrameResult_Suboptimal) [[unlikely]] {
        std::invoke(std::forward<Rebuild>(rebuild));
    }

    if constexpr ((N & (N - 1)) == 0) {
        frameIndex = (frameIndex + 1) & (N - 1);
    } else if constexpr (N == 3) {
        frameIndex = (frameIndex == 2) ? 0 : frameIndex + 1;
    } else {
        frameIndex = (frameIndex + 1) % N;
    }

    return result;
}

inline auto SubmitAndPresent(const ZHLN_FrameSubmitDesc& desc) noexcept -> ZHLN_FrameResult {
    return ZHLN_SubmitAndPresent(&desc);
}

inline void ExecuteCommands(const VkCommandBuffer primary, const std::span<const VkCommandBuffer> secondaries) noexcept {
    if (!secondaries.empty()) {
        vkCmdExecuteCommands(primary, static_cast<uint32_t>(secondaries.size()), secondaries.data());
    }
}

inline std::expected<VkResult, std::string> CheckResult(const VkResult result, const char* context, const std::source_location location) {
    if (result != VK_SUCCESS) [[unlikely]] {
        return std::unexpected(ReportVkError(result, context, location));
    }
    return result;
}

// NOTE: these probes MUST enumerate the complete extension list.
// They used to read into a fixed std::array<..., maxInstanceExtensions> (128)
// and clamp the count, which silently hid every extension the driver reported
// past index 127. Current desktop drivers expose far more than that (NVIDIA
// ships >200 device extensions), and the truncation was order-dependent: the
// KHR ray-tracing trio survived the cut while VK_EXT_mesh_shader did not, so
// mesh shading was reported as unsupported on hardware that fully supports it.
// ExtensionBuilder::ForDevice() always used a growable vector, which is why
// Require(VK_EXT_descriptor_heap) kept working and masked the bug.

inline auto EnumerateInstanceExtensions() noexcept -> std::vector<VkExtensionProperties> {
    std::vector<VkExtensionProperties> available;
    VkResult                           result = VK_INCOMPLETE;

    // Loop: the count can grow between the sizing call and the fetch call.
    while (result == VK_INCOMPLETE) {
        uint32_t count = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS || count == 0) {
            return {};
        }
        available.resize(count);
        result = vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
        if (result == VK_SUCCESS) {
            available.resize(count);
        }
    }
    return available;
}

inline auto EnumerateDeviceExtensions(VkPhysicalDevice physical) noexcept -> std::vector<VkExtensionProperties> {
    std::vector<VkExtensionProperties> available;
    VkResult                           result = VK_INCOMPLETE;

    while (result == VK_INCOMPLETE) {
        uint32_t count = 0;
        if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr) != VK_SUCCESS || count == 0) {
            return {};
        }
        available.resize(count);
        result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data());
        if (result == VK_SUCCESS) {
            available.resize(count);
        }
    }
    return available;
}

inline auto IsInstanceExtensionSupported(std::string_view extension) noexcept -> bool {
    for (const auto& available: EnumerateInstanceExtensions()) {
        if (extension == available.extensionName) {
            return true;
        }
    }
    return false;
}

inline auto IsDeviceExtensionSupported(VkPhysicalDevice physical, std::string_view extension) noexcept -> bool {
    for (const auto& available: EnumerateDeviceExtensions(physical)) {
        if (extension == available.extensionName) {
            return true;
        }
    }
    return false;
}

inline void Dispatch(VkCommandBuffer cmd, uint32_t totalX, uint32_t totalY, uint32_t totalZ, uint32_t localX, uint32_t localY, uint32_t localZ) noexcept {
    ZHLN_CmdDispatch(cmd, (totalX + localX - 1) / localX, (totalY + localY - 1) / localY, (totalZ + localZ - 1) / localZ);
}

inline void DispatchGroups(VkCommandBuffer cmd, uint32_t gX, uint32_t gY, uint32_t gZ) noexcept {
    ZHLN_CmdDispatch(cmd, gX, gY, gZ);
}

template <uint32_t Width, uint32_t Height>
consteval auto GetMipLevels() noexcept -> uint32_t {
    return std::bit_width(ZHLN::Max(Width, Height));
}

inline void GenerateMipmaps(const VkCommandBuffer cmd, const VkImage image, const uint32_t width, const uint32_t height) {
    uint32_t levels = std::bit_width(ZHLN::Max(width, height));
    ZHLN_GenerateMipmaps(cmd, image, static_cast<int32_t>(width), static_cast<int32_t>(height), levels);
}

} // namespace ZHLN::Vk
