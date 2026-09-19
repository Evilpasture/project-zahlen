// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "RenderCore.hpp"
#include <Zahlen/Core/Math.hpp>

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

inline CommandBufferGuard::CommandBufferGuard(VkCommandBuffer cmdBuffer) noexcept: cmd(cmdBuffer) {
    const VkCommandBufferBeginInfo info = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    vkBeginCommandBuffer(cmd, &info);
}

inline CommandBufferGuard::CommandBufferGuard(VkCommandBuffer cmdBuffer, const VkCommandBufferBeginInfo& info) noexcept: cmd(cmdBuffer) {
    vkBeginCommandBuffer(cmd, &info);
}

inline CommandBufferGuard::~CommandBufferGuard() noexcept {
    End();
}

inline CommandBufferGuard::CommandBufferGuard(CommandBufferGuard&& other) noexcept: cmd(std::exchange(other.cmd, VK_NULL_HANDLE)) {
}

inline auto CommandBufferGuard::operator=(CommandBufferGuard&& other) noexcept -> CommandBufferGuard& {
    if (this != &other) {
        End();
        cmd = std::exchange(other.cmd, VK_NULL_HANDLE);
    }
    return *this;
}

inline void CommandBufferGuard::End() noexcept {
    if (cmd != VK_NULL_HANDLE) {
        vkEndCommandBuffer(cmd);
        cmd = VK_NULL_HANDLE;
    }
}

inline void ImageBarrier(const VkCommandBuffer cmd, const ZHLN_ImageBarrierDesc& desc) noexcept {
    const VkImageMemoryBarrier2 barrier = MakeImageBarrier(desc);
    PipelineBarrier(cmd, {}, std::span<const VkImageMemoryBarrier2>(&barrier, 1));
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
// offset 0, so per-draw structs are pushed at offset 0. Higher offsets are
// reflected from the shared Slang push-data layout and hold per-frame data
// consumed by VkDescriptorSetAndBindingMappingEXT sources such as
// VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT.
//
// These two write bytes and ask nothing; they are the primitive, not the API.
// A caller with a push struct names the shader module(s) that read it through
// the wrappers that hold it against them -- `PushHeapData` for a bare write,
// the `Dispatch*` / `Execute*` / `Draw*` entry points for a dispatch or a
// draw.

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

inline auto PresentFrame(const ZHLN_PresentDesc& desc) noexcept -> std::expected<void, ErrorCode> {
    // One implementation: this used to repeat ZHLN_PresentFrame's switch over
    // vkQueuePresentKHR, so the C and C++ spellings of the same call could (and
    // did) drift. The C function is the call; this is its std::expected face,
    // and ToFrameError is the one place its result becomes an ErrorCode.
    const VkResult result = ZHLN_PresentFrame(&desc);
    if (result == VK_SUCCESS) {
        return {};
    }
    return std::unexpected(ToFrameError(result));
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

namespace TemplatedDetail {

template <typename Enumerate>
[[nodiscard]] auto EnumerateExtensionProperties(Enumerate&& enumerate) noexcept -> std::vector<VkExtensionProperties> {
    std::vector<VkExtensionProperties> available;
    VkResult                           result = VK_INCOMPLETE;
    while (result == VK_INCOMPLETE) {
        uint32_t count = 0;
        if (enumerate(&count, nullptr) != VK_SUCCESS || count == 0) {
            return {};
        }
        available.resize(count);
        result = enumerate(&count, available.data());
        if (result == VK_SUCCESS) {
            available.resize(count);
            return available;
        }
        if (result != VK_INCOMPLETE) {
            return {};
        }
    }
    return {};
}

[[nodiscard]] inline auto ExtensionNames(const std::vector<VkExtensionProperties>& props) -> std::vector<std::string> {
    std::vector<std::string> names;
    names.reserve(props.size());
    for (const auto& prop: props) {
        names.emplace_back(prop.extensionName);
    }
    return names;
}

} // namespace TemplatedDetail

inline auto EnumerateInstanceExtensions() noexcept -> std::vector<VkExtensionProperties> {
    // Can run before any instance exists: acquire the Vulkan loader through
    // Volk before touching the dispatch pointers.
    if (ZHLN_EnsureVulkanLoader() != VK_SUCCESS) {
        return {};
    }
    return TemplatedDetail::EnumerateExtensionProperties([](uint32_t* count, VkExtensionProperties* props) {
        return vkEnumerateInstanceExtensionProperties(nullptr, count, props);
    });
}

inline auto EnumerateDeviceExtensions(VkPhysicalDevice physical) noexcept -> std::vector<VkExtensionProperties> {
    return TemplatedDetail::EnumerateExtensionProperties([physical](uint32_t* count, VkExtensionProperties* props) {
        return vkEnumerateDeviceExtensionProperties(physical, nullptr, count, props);
    });
}

[[nodiscard]] inline auto HasExtension(std::span<const VkExtensionProperties> available, std::string_view name) noexcept -> bool {
    for (const auto& prop: available) {
        if (name == prop.extensionName) {
            return true;
        }
    }
    return false;
}

inline auto IsInstanceExtensionSupported(std::string_view extension) noexcept -> bool {
    return HasExtension(EnumerateInstanceExtensions(), extension);
}

template <typename... Names>
[[nodiscard]] inline auto QueryDeviceExtensions(VkPhysicalDevice physical, const Names&... names) noexcept -> ExtensionQuery<sizeof...(Names)> {
    const auto available = EnumerateDeviceExtensions(physical);
    return ExtensionQuery<sizeof...(Names)> {
        .names         = {std::string_view(names)...},
        .present       = {HasExtension(available, std::string_view(names))...},
        .reportedCount = static_cast<uint32_t>(available.size()),
    };
}

// Defined after the template above on purpose: this calls into it, and a
// function template must not be instantiated before its definition is visible.
inline auto IsDeviceExtensionSupported(VkPhysicalDevice physical, std::string_view extension) noexcept -> bool {
    return QueryDeviceExtensions(physical, extension).All();
}

inline void DispatchGroups(VkCommandBuffer cmd, uint32_t gX, uint32_t gY, uint32_t gZ) noexcept {
    vkCmdDispatch(cmd, gX, gY, gZ);
}

inline void Dispatch(VkCommandBuffer cmd, uint32_t totalX, uint32_t totalY, uint32_t totalZ, uint32_t localX, uint32_t localY, uint32_t localZ) noexcept {
    DispatchGroups(cmd, (totalX + localX - 1) / localX, (totalY + localY - 1) / localY, (totalZ + localZ - 1) / localZ);
}

constexpr auto GetMipLevels(uint32_t width, uint32_t height) noexcept -> uint32_t {
    return std::bit_width(ZHLN::Math::Max(width, height));
}

template <uint32_t Width, uint32_t Height>
consteval auto GetMipLevels() noexcept -> uint32_t {
    return GetMipLevels(Width, Height);
}

inline void GenerateMipmaps(const VkCommandBuffer cmd, const VkImage image, const uint32_t width, const uint32_t height) {
    ZHLN_GenerateMipmaps(cmd, image, static_cast<int32_t>(width), static_cast<int32_t>(height), GetMipLevels(width, height));
}

} // namespace ZHLN::Vk
