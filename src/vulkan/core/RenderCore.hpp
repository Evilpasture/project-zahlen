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

#include "FrameStorage.hpp"
#include "RenderCore.h"

#include "../VkError.hpp"

namespace ZHLN {

struct Color4 {
    float r, g, b, a;
};

}

// NOLINTBEGIN(misc-misplaced-const, readability-avoid-const-params-in-decls)

namespace ZHLN::Vk {

enum class VulkanCallError : uint8_t {
    VulkanCallFailed ZHLN_ANNOTATION(ZHLN::Description<"Vulkan call failed">{}) = 1,
};


template <typename T>
concept GpuTriviallyCopyable = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;


// A runtime-format pipeline remains available for legacy render paths. A
// format-specialized pipeline can only be created by the corresponding typed
// PipelineBuilder; its attachment formats are not a caller-supplied label.
struct RuntimeAttachmentFormats {};

template <VkFormat DepthFormat, VkFormat... ColorFormats>
struct AttachmentFormats {
    static constexpr VkFormat depth_format = DepthFormat;
    static constexpr std::array<VkFormat, sizeof...(ColorFormats)> color_formats {ColorFormats...};
};

template <typename Formats, VkFormat... Added>
struct AppendAttachmentColors {
    using type = RuntimeAttachmentFormats;
};

template <VkFormat Depth, VkFormat... Colors, VkFormat... Added>
struct AppendAttachmentColors<AttachmentFormats<Depth, Colors...>, Added...> {
    using type = std::conditional_t<
        ((Added != VK_FORMAT_UNDEFINED) && ...), AttachmentFormats<Depth, Colors..., Added...>, RuntimeAttachmentFormats
    >;
};

template <typename Formats, VkFormat Depth>
struct SetAttachmentDepth {
    using type = RuntimeAttachmentFormats;
};

template <VkFormat OldDepth, VkFormat... Colors, VkFormat Depth>
struct SetAttachmentDepth<AttachmentFormats<OldDepth, Colors...>, Depth> {
    using type = std::conditional_t<Depth == VK_FORMAT_UNDEFINED, RuntimeAttachmentFormats, AttachmentFormats<Depth, Colors...>>;
};

template <typename Formats>
struct WithoutAttachmentDepth {
    using type = RuntimeAttachmentFormats;
};

template <VkFormat Depth, VkFormat... Colors>
struct WithoutAttachmentDepth<AttachmentFormats<Depth, Colors...>> {
    using type = AttachmentFormats<VK_FORMAT_UNDEFINED, Colors...>;
};

template <typename Formats>
struct ClearAttachmentColors {
    using type = RuntimeAttachmentFormats;
};

template <VkFormat Depth, VkFormat... Colors>
struct ClearAttachmentColors<AttachmentFormats<Depth, Colors...>> {
    using type = AttachmentFormats<Depth>;
};

template <size_t ColorCount, bool HasDepth, typename Formats>
class PipelineBuilder;

template <size_t ColorCount, bool HasDepth, typename Formats = RuntimeAttachmentFormats>
class TypedPipeline {
  public:
    using FormatSet = Formats;

    TypedPipeline() = default;
    explicit TypedPipeline(Pipeline&& p) noexcept requires std::same_as<Formats, RuntimeAttachmentFormats>: handle(std::move(p)) {
    }

    auto operator=(Pipeline&& p) noexcept -> TypedPipeline& requires std::same_as<Formats, RuntimeAttachmentFormats> {
        handle = std::move(p);
        return *this;
    }

    [[nodiscard]] auto Get() const noexcept -> VkPipeline {
        return handle.Get();
    }
    [[nodiscard]] auto Valid() const noexcept -> bool {
        return handle.Valid();
    }
    explicit operator bool() const noexcept {
        return Valid();
    }

    [[nodiscard]] auto Release() noexcept -> Pipeline {
        return std::move(handle);
    }

  private:
    template <size_t, bool, typename>
    friend class PipelineBuilder;
    struct BuilderToken {};
    explicit TypedPipeline(Pipeline&& p, BuilderToken) noexcept: handle(std::move(p)) {
    }

    Pipeline handle;
};

inline constexpr auto& GetBufferAddress = ZHLN_GetBufferDeviceAddress;

[[nodiscard]] std::expected<void, ErrorCode> WaitIdle(VkDevice device) noexcept;


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

[[nodiscard]] constexpr auto MakeCommandBufferSubmitInfo(VkCommandBuffer cmd) noexcept -> VkCommandBufferSubmitInfo {
    return {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cmd};
}

inline constexpr VkPipelineStageFlags2 kAsyncComputeConsumerStages =
    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

[[nodiscard]] constexpr auto MakeSemaphoreSubmitInfo(VkSemaphore semaphore, uint64_t value, VkPipelineStageFlags2 stage) noexcept -> VkSemaphoreSubmitInfo {
    return {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = semaphore, .value = value, .stageMask = stage};
}

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

[[nodiscard]] constexpr auto ToFrameError(const VkResult result) noexcept -> ErrorCode {
    const Vk::Error err {result};
    if (result == VK_ERROR_DEVICE_LOST) {
        return ErrorCode {FrameResult::DeviceLost};
    }
    return err;
}
[[nodiscard]] auto PresentFrame(const ZHLN_PresentDesc& desc) noexcept -> FrameOutcome<PresentSuboptimal>;

void ExecuteCommands(const VkCommandBuffer primary, const std::span<const VkCommandBuffer> secondaries) noexcept;


[[nodiscard]] std::string ReportVkError(VkResult result, const char* context, const std::source_location& location);
[[noreturn]] void         ReportSemaphoreBoundsError(uint32_t index, uint32_t count) noexcept;

[[nodiscard]] std::expected<VkResult, std::string>
    CheckResult(const VkResult result, const char* context = "", const std::source_location location = std::source_location::current());


[[nodiscard]] auto EnumerateInstanceExtensions() noexcept -> std::vector<VkExtensionProperties>;
[[nodiscard]] auto EnumerateDeviceExtensions(VkPhysicalDevice physical) noexcept -> std::vector<VkExtensionProperties>;

[[nodiscard]] auto IsInstanceExtensionSupported(std::string_view extension) noexcept -> bool;
[[nodiscard]] auto IsDeviceExtensionSupported(VkPhysicalDevice physical, std::string_view extension) noexcept -> bool;

template <size_t N>
struct ExtensionQuery {
    std::array<std::string_view, N> names {};
    std::array<bool, N>             present {};
    uint32_t                        reportedCount = 0;

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

    [[nodiscard]] constexpr auto operator[](const size_t index) const noexcept -> bool {
        return index < N && present[index];
    }

    [[nodiscard]] constexpr auto FirstMissing() const noexcept -> std::string_view {
        for (size_t i = 0; i < N; ++i) {
            if (!present[i]) {
                return names[i];
            }
        }
        return {};
    }
};

template <typename... Names>
[[nodiscard]] auto QueryDeviceExtensions(VkPhysicalDevice physical, const Names&... names) noexcept -> ExtensionQuery<sizeof...(Names)>;

void Dispatch(VkCommandBuffer cmd, uint32_t totalX, uint32_t totalY, uint32_t totalZ, uint32_t localX, uint32_t localY, uint32_t localZ) noexcept;
void DispatchGroups(VkCommandBuffer cmd, uint32_t gX, uint32_t gY, uint32_t gZ) noexcept;


[[nodiscard]] constexpr auto GetMipLevels(uint32_t width, uint32_t height) noexcept -> uint32_t;

template <uint32_t Width, uint32_t Height>
consteval auto GetMipLevels() noexcept -> uint32_t;

void GenerateMipmaps(const VkCommandBuffer cmd, const VkImage image, const uint32_t width, const uint32_t height);

// NOLINTEND(misc-misplaced-const, readability-avoid-const-params-in-decls)

}

#include "RenderCore.inl"
