// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/FrameResult.hpp>
#include <cstdint>

// C layer twin: brings Volk's declarations (and, through it, the Vulkan
// headers) plus the ZHLN_* entry points used by the helpers below.
#include "RenderCore.h"

namespace ZHLN {

struct Color4 {
    float r, g, b, a;
};

// NOLINTBEGIN(misc-misplaced-const, readability-avoid-const-params-in-decls)

template <typename T>
struct PerFrame {
    std::array<T, 2> data {};
    uint32_t         idx = 0;

    PerFrame() = default;

    constexpr PerFrame(T first, T second) noexcept: data {{std::move(first), std::move(second)}} {
    }

    // C++23 Zero-Argument Subscript Overload for []
    [[nodiscard]] constexpr T& operator[]() noexcept {
        return data[idx];
    }
    [[nodiscard]] constexpr const T& operator[]() const noexcept {
        return data[idx];
    }

    // Standard Single-Argument Subscript Overload for [i]
    [[nodiscard]] constexpr T& operator[](uint32_t i) noexcept {
        return data[i % 2];
    }
    [[nodiscard]] constexpr const T& operator[](uint32_t i) const noexcept {
        return data[i % 2];
    }

    // Keep existing pointer and helper APIs
    [[nodiscard]] constexpr T& operator*() noexcept {
        return data[idx];
    }
    [[nodiscard]] constexpr const T& operator*() const noexcept {
        return data[idx];
    }
    [[nodiscard]] constexpr T* operator->() noexcept {
        return &data[idx];
    }
    [[nodiscard]] constexpr const T* operator->() const noexcept {
        return &data[idx];
    }
    [[nodiscard]] constexpr T& Current() noexcept {
        return data[idx];
    }
    [[nodiscard]] constexpr const T& Current() const noexcept {
        return data[idx];
    }
    [[nodiscard]] constexpr T& Next() noexcept {
        return data[idx ^ 1];
    }
    [[nodiscard]] constexpr const T& Next() const noexcept {
        return data[idx ^ 1];
    }

    void Advance() noexcept {
        idx ^= 1;
    }
    void Flip() noexcept {
        idx ^= 1;
    }
};

template <typename T>
using DoubleBuffered = PerFrame<T>;

// C++20/C++23 Concepts to evaluate layout capabilities at compile-time
template <typename T>
concept CanFlipDirect = requires(T& t) { t.Flip(); };

template <typename T>
concept CanFlipIterable = requires(T& t) {
    requires !CanFlipDirect<T>;
    t.begin();
    t.end();
    requires requires(typename T::value_type& item) { item.Flip(); };
};

inline void FlipObject(auto& obj) noexcept {
    if constexpr (CanFlipDirect<decltype(obj)>) {
        obj.Flip();
    } else if constexpr (CanFlipIterable<decltype(obj)>) {
        for (auto& item: obj) {
            item.Flip();
        }
    }
}

} // namespace ZHLN

namespace ZHLN::Vk {

// Raised by low-level Vulkan call wrappers for a failure that has no result
// code to forward -- a call that reports failure by returning a null handle,
// for instance (see the acceleration-structure build in RenderResources.cpp).
// Where a VkResult *is* available, it is the error (ToFrameError, next to the
// frame verbs) rather than a name invented here: a lost device is
// FrameResult::DeviceLost, and everything else is whatever the driver said.
// Stays inside the RHI layer: content/asset code must not branch on it.
enum class VulkanCallError : uint8_t {
    VulkanCallFailed ZHLN_ANNOTATION(ZHLN::Description<"Vulkan call failed">{}) = 1,
};

// TMP / Concepts

template <typename T>
concept GpuTriviallyCopyable = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

// Type safe Pipeline

template <size_t ColorCount, bool HasDepth>
class TypedPipeline {
  public:
    Pipeline handle;

    TypedPipeline() = default;
    explicit TypedPipeline(Pipeline&& p) noexcept: handle(std::move(p)) {
    }

    // Allow move assignment from raw legacy Pipeline
    TypedPipeline& operator=(Pipeline&& p) noexcept {
        handle = std::move(p);
        return *this;
    }

    [[nodiscard]] VkPipeline Get() const noexcept {
        return handle.Get();
    }
    [[nodiscard]] bool Valid() const noexcept {
        return handle.Valid();
    }
    explicit operator bool() const noexcept {
        return Valid();
    }

    [[nodiscard]] Pipeline Release() noexcept {
        return std::move(handle);
    }
};

inline constexpr auto& GetBufferAddress = ZHLN_GetBufferDeviceAddress;

[[nodiscard]] std::expected<void, ErrorCode> WaitIdle(VkDevice device) noexcept;

// Scoped RAII Scissor State Guard

struct ScopedScissor {
    VkCommandBuffer commandRect;
    VkRect2D        resetScissor;

    struct ScissorDesc {
        VkRect2D target;
        VkRect2D fallback;
    };
    ScopedScissor(VkCommandBuffer cmd, const ScissorDesc& desc) noexcept;
    ~ScopedScissor() noexcept;

    ScopedScissor(const ScopedScissor&)                    = delete;
    auto operator=(const ScopedScissor&) -> ScopedScissor& = delete;
    ScopedScissor(ScopedScissor&&)                         = delete;
    auto operator=(ScopedScissor&&) -> ScopedScissor&      = delete;
};

// Command & Rendering Helpers

class ScopedRendering {
  public:
    ScopedRendering(const VkCommandBuffer cmd, const ZHLN_RenderPassDesc& desc) noexcept;
    ~ScopedRendering() noexcept;

    ScopedRendering(ScopedRendering&&)                         = delete;
    auto operator=(ScopedRendering&&) -> ScopedRendering&      = delete;
    ScopedRendering(const ScopedRendering&)                    = delete;
    auto operator=(const ScopedRendering&) -> ScopedRendering& = delete;

  private:
    VkCommandBuffer _cmd;
};

// Begins a command buffer on construction and ends it on destruction.
// Default begin is one-time-submit with no inheritance (primary). Pass a
// VkCommandBufferBeginInfo for secondaries. End() is idempotent so a split
// record/submit can close the buffer before the destructor runs.
class CommandBufferGuard {
  public:
    explicit CommandBufferGuard(VkCommandBuffer cmdBuffer) noexcept;
    CommandBufferGuard(VkCommandBuffer cmdBuffer, const VkCommandBufferBeginInfo& info) noexcept;
    ~CommandBufferGuard() noexcept;

    void End() noexcept;

    [[nodiscard]] VkCommandBuffer get() const noexcept {
        return cmd;
    }

    CommandBufferGuard(const CommandBufferGuard&)            = delete;
    CommandBufferGuard& operator=(const CommandBufferGuard&) = delete;
    CommandBufferGuard(CommandBufferGuard&& other) noexcept;
    CommandBufferGuard& operator=(CommandBufferGuard&& other) noexcept;

  private:
    VkCommandBuffer cmd {};
};

void ImageBarrier(const VkCommandBuffer cmd, const ZHLN_ImageBarrierDesc& desc) noexcept;

void CopyBufferToImage(const VkCommandBuffer cmd, const ZHLN_BufferImageCopyDesc& desc) noexcept;

void CopyImageToBuffer(
    VkCommandBuffer    cmd,
    VkImage            srcImage,
    VkBuffer           dstBuffer,
    VkExtent2D         extent,
    VkImageLayout      layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT
) noexcept;

template <size_t RegionCount>
[[nodiscard]] constexpr auto CreateCopyRegions(
    VkDeviceSize       baseOffset,
    VkDeviceSize       regionSize,
    VkExtent3D         extent,
    VkImageAspectFlags aspect         = VK_IMAGE_ASPECT_COLOR_BIT,
    uint32_t           mipLevel       = 0,
    uint32_t           baseArrayLayer = 0
) noexcept -> std::array<VkBufferImageCopy2, RegionCount>;

template <size_t RegionCount>
inline void CopyBufferToImage(
    VkCommandBuffer                                    cmd,
    VkBuffer                                           srcBuffer,
    VkImage                                            dstImage,
    const std::array<VkBufferImageCopy2, RegionCount>& regions,
    VkImageLayout                                      layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
) noexcept;

template <GpuTriviallyCopyable T>
void Push(const VkCommandBuffer cmd, const VkPipelineLayout layout, const VkShaderStageFlags stages, const T& value) noexcept;

// Frame Execution
[[nodiscard]] constexpr auto MakeCommandBufferSubmitInfo(VkCommandBuffer cmd) noexcept -> VkCommandBufferSubmitInfo {
    return {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cmd};
}

// Pipeline stages of a graphics submission that consume the async compute frame's output; used
// as the wait stage when submitting behind the compute timeline, so it must name the
// *earliest* consumer.
//
// A destination stage mask only implies logically later stages, so the old
// VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT never ordered draw-indirect fetches, vertex input or
// the vertex/task/mesh shaders against the compute submission -- and those are real consumers:
// the 2D and mesh particle renderers read the particle buffer the update passes write on the
// compute queue. DRAW_INDIRECT and VERTEX_INPUT are named because they would fetch culling
// output if that ever moves off the graphics queue: being early costs overlap, being late is a
// data race.
inline constexpr VkPipelineStageFlags2 kAsyncComputeConsumerStages =
    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;

[[nodiscard]] constexpr auto MakeSemaphoreSubmitInfo(VkSemaphore semaphore, uint64_t value, VkPipelineStageFlags2 stage) noexcept -> VkSemaphoreSubmitInfo {
    return {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = semaphore, .value = value, .stageMask = stage};
}

// One vkQueueSubmit2. Empty spans are omitted. This is the only C++ caller of vkQueueSubmit2.
[[nodiscard]] std::expected<void, ErrorCode> QueueSubmit(
    VkQueue                                        queue,
    std::span<const VkCommandBufferSubmitInfo>     cmds,
    std::span<const VkSemaphoreSubmitInfo>         waits   = {},
    std::span<const VkSemaphoreSubmitInfo>         signals = {},
    VkFence                                        fence   = VK_NULL_HANDLE
) noexcept;

[[nodiscard]] std::expected<void, ErrorCode> QueueSubmit(
    VkQueue               queue,
    VkCommandBuffer       cmd,
    VkSemaphore           waitSemaphore   = VK_NULL_HANDLE,
    uint64_t              waitValue       = 0,
    VkPipelineStageFlags2 waitStage       = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    VkSemaphore           signalSemaphore = VK_NULL_HANDLE,
    uint64_t              signalValue     = 0,
    VkPipelineStageFlags2 signalStage     = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    VkFence               fence           = VK_NULL_HANDLE
) noexcept;

template <QueueType QType>
[[nodiscard]] inline std::expected<void, ErrorCode> QueueSubmit(
    const Context&        ctx,
    CommandBuffer<QType>  cmd,
    VkSemaphore           waitSemaphore   = VK_NULL_HANDLE,
    uint64_t              waitValue       = 0,
    VkPipelineStageFlags2 waitStage       = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    VkSemaphore           signalSemaphore = VK_NULL_HANDLE,
    uint64_t              signalValue     = 0,
    VkPipelineStageFlags2 signalStage     = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    VkFence               fence           = VK_NULL_HANDLE
) noexcept {
    return QueueSubmit(ResolveQueue<QType>(ctx), cmd.handle, waitSemaphore, waitValue, waitStage, signalSemaphore, signalValue, signalStage, fence);
}

// The frame path's single VkResult -> ErrorCode mapping, and the reason no std::expected in
// this layer has a VkResult for its error.
//
// *Errors* only: VK_ERROR_DEVICE_LOST gets the frame vocabulary's name
// (FrameResult::DeviceLost, so the caller rebuilds the device), VK_SUCCESS maps to the zero
// code (a caller returns an engaged expected for it instead), and everything else keeps the
// driver's own code with category "VkResult". The two results that are *not* errors --
// VK_SUBOPTIMAL_KHR and VK_ERROR_OUT_OF_DATE_KHR -- deliberately do not appear here: the verbs
// that can see them (PresentFrame, AcquireNext) turn them into their own non-failure first, so
// a non-failure can never be constructed into an error slot through this door.
[[nodiscard]] constexpr auto ToFrameError(const VkResult result) noexcept -> ErrorCode {
    switch (result) {
        case VK_SUCCESS:
            return {};
        case VK_ERROR_DEVICE_LOST:
            return ErrorCode {FrameResult::DeviceLost};
        default:
            return ErrorCode {result};
    }
}

// vkQueuePresentKHR, through the C layer, as FrameOutcome: engaged with
// std::nullopt means the image went to the presentation engine; engaged with
// PresentSuboptimal means it did not go through as asked and the caller should
// rebuild and draw again (see that type for why it is not an error); otherwise
// error() is what ToFrameError made of the call's result.
[[nodiscard]] auto PresentFrame(const ZHLN_PresentDesc& desc) noexcept -> FrameOutcome<PresentSuboptimal>;

void ExecuteCommands(const VkCommandBuffer primary, const std::span<const VkCommandBuffer> secondaries) noexcept;

// Error Helpers

[[nodiscard]] std::string ReportVkError(VkResult result, const char* context, const std::source_location& location);
[[noreturn]] void         ReportSemaphoreBoundsError(uint32_t index, uint32_t count) noexcept;

[[nodiscard]] std::expected<VkResult, std::string>
    CheckResult(const VkResult result, const char* context = "", const std::source_location location = std::source_location::current());

// Extension Query Utilities

// Full, untruncated enumerations. Never size these with a fixed array: drivers
// routinely report >200 device extensions and clamping the count silently
// hides everything past the cut-off (see the note in RenderCore.inl).
[[nodiscard]] auto EnumerateInstanceExtensions() noexcept -> std::vector<VkExtensionProperties>;
[[nodiscard]] auto EnumerateDeviceExtensions(VkPhysicalDevice physical) noexcept -> std::vector<VkExtensionProperties>;

[[nodiscard]] auto IsInstanceExtensionSupported(std::string_view extension) noexcept -> bool;
[[nodiscard]] auto IsDeviceExtensionSupported(VkPhysicalDevice physical, std::string_view extension) noexcept -> bool;

// Answer for a fixed set of extension names obtained from ONE enumeration.
// IsDeviceExtensionSupported above re-enumerates on every call, and drivers
// routinely report >200 device extensions, so asking about N extensions one at
// a time costs N full two-pass enumerations plus N vector allocations. Ask once.
template <size_t N>
struct ExtensionQuery {
    std::array<std::string_view, N> names {};
    std::array<bool, N>             present {};
    uint32_t                        reportedCount = 0;

    // True when every queried name is present. Vacuously true when N == 0.
    [[nodiscard]] constexpr auto All() const noexcept -> bool {
        for (size_t i = 0; i < N; ++i) {
            if (!present[i]) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] constexpr auto Any() const noexcept -> bool {
        for (size_t i = 0; i < N; ++i) {
            if (present[i]) {
                return true;
            }
        }
        return false;
    }

    // Positional: result[0] is the first name passed to the query.
    [[nodiscard]] constexpr auto operator[](const size_t index) const noexcept -> bool {
        return index < N && present[index];
    }

    // The first queried name the device did not report, or an empty view when
    // every one of them is there. Meant for the "X not present among the N
    // device extensions reported" style of log line.
    [[nodiscard]] constexpr auto FirstMissing() const noexcept -> std::string_view {
        for (size_t i = 0; i < N; ++i) {
            if (!present[i]) {
                return names[i];
            }
        }
        return {};
    }
};

// Queries a physical device for every name in one enumeration. Accepts
// anything convertible to std::string_view: string literals, const char*, the
// VK_*_EXTENSION_NAME macros and std::string_view itself.
template <typename... Names>
[[nodiscard]] auto QueryDeviceExtensions(VkPhysicalDevice physical, const Names&... names) noexcept -> ExtensionQuery<sizeof...(Names)>;

void Dispatch(VkCommandBuffer cmd, uint32_t totalX, uint32_t totalY, uint32_t totalZ, uint32_t localX, uint32_t localY, uint32_t localZ) noexcept;
void DispatchGroups(VkCommandBuffer cmd, uint32_t gX, uint32_t gY, uint32_t gZ) noexcept;

// Mipmapping

[[nodiscard]] constexpr auto GetMipLevels(uint32_t width, uint32_t height) noexcept -> uint32_t;

template <uint32_t Width, uint32_t Height>
consteval auto GetMipLevels() noexcept -> uint32_t;

void GenerateMipmaps(const VkCommandBuffer cmd, const VkImage image, const uint32_t width, const uint32_t height);

// NOLINTEND(misc-misplaced-const, readability-avoid-const-params-in-decls)

} // namespace ZHLN::Vk

#include "RenderCore.inl"
