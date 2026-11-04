// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ZHLN::VkD3D barriers -- the Tell-Don't-Ask synchronization layer. A
// barrier is a compact value that names WHAT hazard is being resolved and
// accepts the domain objects that already know their identity/extent
// (image views/slices, buffers/buffer slices) through structural factory
// protocols; callers never unpack handles, subresource ranges, or
// buffer offset/size triples at the call site. Recording is a member
// verb: the backend TDA header defines Record() for its command type
// (src/vkd3d/include/vkd3d/VulkanBarriers.hpp for VkCommandBuffer through
// vkCmdPipelineBarrier2; src/vkd3d/D3D12Barriers.hpp lowers the same
// values to enhanced barriers taking an ID3D12Resource*). Neither backend
// names these types; src/vulkan and src/d3d12 keep their own native
// barrier code.
//
// Resource identity: BufferBarrier/ImageBarrier carry an opaque 64-bit
// resource word. On the Vulkan path the word IS the VkBuffer/VkImage value
// (both are 64-bit non-dispatchable handles). D3D12 synthesis takes its
// ID3D12Resource* as a separate argument and ignores the word. The
// queue-family fields are likewise Vulkan-only (queue-ownership transfer);
// D3D12 synthesis leaves them unread.
//
// Layout-to-sync derivation (GetLayoutSyncInfo / LayoutTraits) is graph
// theory, not backend knowledge: the stage/access a layout implies is the
// same fact on both APIs, so it lives here and both sides consume it.

#include <vkd3d/Enums.hpp>

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace ZHLN::VkD3D {

// -----------------------------------------------------------------------------
// Tell-Don't-Ask domain protocols
// -----------------------------------------------------------------------------
// The barrier classes are the TDA layer: callers hand over the domain object
// that already knows its identity and extent (an image view/slice, a buffer
// or buffer slice) and state the hazard they want resolved. The factories
// below ask that object for its handle/range internally; call sites never
// unpack Handle()/SubresourceRange()/buffer-offset-size themselves. These
// protocols are structural (concepts), so this header names no backend domain
// type -- Vk::ImageView, Vk::ImageSlice, Vk::Buffer and Vk::BufferSlice all
// satisfy them from src/vulkan without src/vulkan naming a VkD3D type.
namespace TemplatedDetail {

/// Opaque 64-bit resource word for a domain handle: integers pass through,
/// non-dispatchable handles (pointer-typed under VK_USE_64_BIT_PTR_DEFINES)
/// are bit-cast. In consteval contexts only null identity is preserved.
[[nodiscard]] constexpr auto ResourceWord(auto handle) noexcept -> std::uint64_t {
    using Handle = std::remove_cvref_t<decltype(handle)>;
    if constexpr (std::is_integral_v<Handle> || std::is_enum_v<Handle>) {
        return static_cast<std::uint64_t>(handle);
    } else if constexpr (std::is_pointer_v<Handle>) {
        if consteval {
            return handle == nullptr ? std::uint64_t {0} : ~std::uint64_t {0};
        }
        return std::bit_cast<std::uint64_t>(handle);
    } else {
        return static_cast<std::uint64_t>(handle);
    }
}

/// Aspect mask bits in the neutral ImageAspect vocabulary (the conversion
/// anchors pin this bit correspondence against both backends).
[[nodiscard]] constexpr auto AspectFromMask(auto mask) noexcept -> ImageAspect {
    const auto bits   = static_cast<std::uint32_t>(mask);
    ImageAspect result = ImageAspect::None;
    if ((bits & static_cast<std::uint32_t>(ImageAspect::Color)) != 0) {
        result = result | ImageAspect::Color;
    }
    if ((bits & static_cast<std::uint32_t>(ImageAspect::Depth)) != 0) {
        result = result | ImageAspect::Depth;
    }
    if ((bits & static_cast<std::uint32_t>(ImageAspect::Stencil)) != 0) {
        result = result | ImageAspect::Stencil;
    }
    return result;
}

/// An image view-like object (Handle + SubresourceRange) or a slice-like
/// object that vends one (View()).
template <typename T>
concept ImageDomain = requires(const T& value) {
    { value.Handle() };
    { value.SubresourceRange() };
} || requires(const T& value) {
    { value.View() };
};

/// A buffer-like object identified by Handle().
template <typename T>
concept BufferDomain = requires(const T& value) {
    { value.Handle() };
};

/// A buffer-slice-like object carrying buffer/offset/size fields.
template <typename T>
concept BufferSliceDomain = requires(const T& value) {
    value.buffer;
    value.offset;
    value.size;
};

} // namespace TemplatedDetail

// -----------------------------------------------------------------------------
// Layout -> synchronization derivation
// -----------------------------------------------------------------------------

struct LayoutSyncInfo {
    PipelineStage stage;
    Access        access;
    std::string_view  name;
};

/// The stage/access pair a layout transition must synchronize against.
/// isSource selects the "work performed in the old layout" side; the
/// destination side is conservative (attachment layouts read+write).
[[nodiscard]] constexpr auto GetLayoutSyncInfo(ImageLayout layout, bool isSource) noexcept -> LayoutSyncInfo {
    switch (layout) {
        case ImageLayout::Undefined:
            return {.stage = PipelineStage::None, .access = Access::None, .name = "UNDEFINED"};

        case ImageLayout::ColorAttachmentOptimal:
            return {
                .stage  = PipelineStage::ColorAttachment,
                .access = isSource ? Access::ColorWrite : (Access::ColorRead | Access::ColorWrite),
                .name   = "COLOR_ATTACHMENT_OPTIMAL",
            };

        case ImageLayout::DepthAttachmentOptimal:
            return {
                .stage  = PipelineStage::EarlyFragment | PipelineStage::LateFragment,
                .access = isSource ? Access::DepthWrite : (Access::DepthRead | Access::DepthWrite),
                .name   = "DEPTH_ATTACHMENT_OPTIMAL",
            };

        case ImageLayout::DepthStencilAttachmentOptimal:
            return {
                .stage  = PipelineStage::EarlyFragment | PipelineStage::LateFragment,
                .access = isSource ? Access::DepthWrite : (Access::DepthRead | Access::DepthWrite),
                .name   = "DEPTH_STENCIL_ATTACHMENT_OPTIMAL",
            };

        case ImageLayout::ShaderReadOnlyOptimal:
            return {
                .stage  = PipelineStage::Fragment | PipelineStage::Compute,
                .access = Access::ShaderRead,
                .name   = "SHADER_READ_ONLY_OPTIMAL",
            };

        case ImageLayout::PresentSrc:
            return {.stage = PipelineStage::ColorAttachment, .access = Access::None, .name = "PRESENT_SRC"};

        case ImageLayout::TransferDstOptimal:
            return {.stage = PipelineStage::Transfer, .access = Access::TransferWrite, .name = "TRANSFER_DST_OPTIMAL"};

        case ImageLayout::TransferSrcOptimal:
            return {.stage = PipelineStage::Transfer, .access = Access::TransferRead, .name = "TRANSFER_SRC_OPTIMAL"};

        case ImageLayout::General:
            return {
                .stage  = PipelineStage::AllCommands,
                .access = Access::ShaderRead | Access::ShaderWrite,
                .name   = "GENERAL",
            };

        default:
            return {
                .stage  = PipelineStage::AllCommands,
                .access = isSource ? Access::MemoryWrite : Access::MemoryRead,
                .name   = "UNKNOWN_OR_CUSTOM_LAYOUT",
            };
    }
}

/// Compile-time layout traits for templated transitions. kStage applies two
/// refinements over the runtime table: shader-read waits only for fragment
/// work by default (compute readers opt in via explicit-stage factories),
/// and General (storage) is anchored at compute.
template <ImageLayout Layout>
struct LayoutTraits {
    static constexpr auto             kInfo = GetLayoutSyncInfo(Layout, false);
    static constexpr std::string_view kName = kInfo.name;

    static constexpr PipelineStage kStage = []() constexpr {
        if constexpr (Layout == ImageLayout::Undefined) {
            return PipelineStage::None;
        } else if constexpr (Layout == ImageLayout::ShaderReadOnlyOptimal) {
            return PipelineStage::Fragment;
        } else if constexpr (Layout == ImageLayout::General) {
            return PipelineStage::Compute;
        } else {
            return kInfo.stage;
        }
    }();

    static constexpr Access kAccess = kInfo.access;
};

// -----------------------------------------------------------------------------
// MemoryBarrier (16 bytes)
// -----------------------------------------------------------------------------
// Global memory/execution hazard: stage/access ordering for all resources.
// Backends lower it to VkMemoryBarrier2 or D3D12_GLOBAL_BARRIER unchanged.
class MemoryBarrier {
  public:
    constexpr MemoryBarrier() noexcept = default;

    constexpr MemoryBarrier(PipelineStage srcStage, Access srcAccess, PipelineStage dstStage, Access dstAccess) noexcept:
        stageBefore(srcStage), accessBefore(srcAccess), stageAfter(dstStage), accessAfter(dstAccess) {
    }

    // -------------------------------------------------------------------------
    // Semantic Intent Factories ("Ask for the hazard you want to resolve")
    // -------------------------------------------------------------------------

    /// Compute shader write to compute shader read (e.g. mip pyramids, Kawase bloom chains).
    [[nodiscard]] static constexpr auto ComputeWriteToRead() noexcept -> MemoryBarrier {
        return {PipelineStage::Compute, Access::ShaderWrite, PipelineStage::Compute, Access::ShaderRead};
    }

    /// Compute shader write to indirect draw argument consumption (e.g. GPU frustum/HiZ culling).
    [[nodiscard]] static constexpr auto ComputeToIndirect() noexcept -> MemoryBarrier {
        return {PipelineStage::Compute, Access::ShaderWrite, PipelineStage::Indirect, Access::IndirectRead};
    }

    /// Compute write to subsequent vertex/fragment shader reads.
    [[nodiscard]] static constexpr auto ComputeToShaderRead() noexcept -> MemoryBarrier {
        return {
            PipelineStage::Compute, Access::ShaderWrite, PipelineStage::Vertex | PipelineStage::Fragment, Access::ShaderRead,
        };
    }

    /// Transfer write to indirect draw argument consumption.
    [[nodiscard]] static constexpr auto TransferToIndirect() noexcept -> MemoryBarrier {
        return {PipelineStage::Transfer | PipelineStage::Copy, Access::TransferWrite, PipelineStage::Indirect, Access::IndirectRead};
    }

    /// Transfer/copy write to subsequent indirect/index/shader/uniform graphics reads.
    [[nodiscard]] static constexpr auto TransferToGraphics() noexcept -> MemoryBarrier {
        return {
            PipelineStage::Copy, Access::TransferWrite,
            PipelineStage::Indirect | PipelineStage::IndexInput | PipelineStage::Vertex | PipelineStage::Fragment,
            Access::IndirectRead | Access::IndexRead | Access::ShaderRead | Access::UniformRead,
        };
    }

    /// Serialize overlapping staged copy writes targeting the same destination buffer.
    [[nodiscard]] static constexpr auto CopyWriteToWrite() noexcept -> MemoryBarrier {
        return {PipelineStage::Copy, Access::TransferWrite, PipelineStage::Copy, Access::TransferWrite};
    }

    /// Staged buffer copy write to BLAS/TLAS build geometry input read.
    [[nodiscard]] static constexpr auto TransferToAccelerationStructureBuild() noexcept -> MemoryBarrier {
        return {PipelineStage::Copy, Access::TransferWrite, PipelineStage::AccelerationStructureBuild, Access::AccelerationStructureRead};
    }

    /// BLAS build write to subsequent TLAS build read.
    [[nodiscard]] static constexpr auto AccelerationStructureBuildToBuild() noexcept -> MemoryBarrier {
        return {
            PipelineStage::AccelerationStructureBuild, Access::AccelerationStructureWrite,
            PipelineStage::AccelerationStructureBuild, Access::AccelerationStructureRead,
        };
    }

    /// TLAS build write to ray-query / trace reads in fragment and compute shaders.
    [[nodiscard]] static constexpr auto AccelerationStructureBuildToTrace() noexcept -> MemoryBarrier {
        return {
            PipelineStage::AccelerationStructureBuild, Access::AccelerationStructureWrite,
            PipelineStage::Fragment | PipelineStage::Compute, Access::ShaderRead | Access::AccelerationStructureRead,
        };
    }

    // -------------------------------------------------------------------------
    // Execution ("Tell the barrier to record"): defined by the backend TDA
    // header for its command type (<vkd3d/VulkanBarriers.hpp> defines these
    // for VkCommandBuffer). Declared here so the value itself is the API.
    // -------------------------------------------------------------------------
    template <typename CommandBuffer>
    void Record(CommandBuffer cmd) const noexcept;

    template <typename CommandBuffer, std::size_t N>
    static void Record(CommandBuffer cmd, const std::array<MemoryBarrier, N>& barriers) noexcept;

    template <typename CommandBuffer>
    static void Record(CommandBuffer cmd, std::span<const MemoryBarrier> barriers) noexcept;

    PipelineStage stageBefore  = PipelineStage::None;
    Access        accessBefore = Access::None;
    PipelineStage stageAfter   = PipelineStage::None;
    Access        accessAfter  = Access::None;
};

static_assert(sizeof(MemoryBarrier) == 16, "MemoryBarrier must be exactly 16 bytes (4 neutral flag words).");
static_assert(std::is_trivially_copyable_v<MemoryBarrier>);

namespace TemplatedDetail {

template <typename T>
    requires BufferSliceDomain<T>
[[nodiscard]] constexpr auto SliceBufferWord(const T& slice) noexcept -> std::uint64_t {
    return ResourceWord(slice.buffer);
}

template <typename T>
    requires BufferSliceDomain<T>
[[nodiscard]] constexpr auto SliceOffset(const T& slice) noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(slice.offset);
}

template <typename T>
    requires BufferSliceDomain<T>
[[nodiscard]] constexpr auto SliceSize(const T& slice) noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(slice.size);
}

template <typename T>
    requires BufferDomain<T>
[[nodiscard]] constexpr auto DomainBufferWord(const T& buffer) noexcept -> std::uint64_t {
    return ResourceWord(buffer.Handle());
}

} // namespace TemplatedDetail

// -----------------------------------------------------------------------------
// BufferBarrier (48 bytes)
// -----------------------------------------------------------------------------
// Buffer-range hazard with optional queue-family ownership transfer
// (Vulkan-only fields; see the header note). The resource word is the
// backend's buffer identity -- on Vulkan, the VkBuffer value.
class BufferBarrier {
  public:
    static constexpr std::uint64_t kWholeSize          = 0xFFFF'FFFF'FFFF'FFFFu;
    static constexpr std::uint32_t kQueueFamilyIgnored = 0xFFFF'FFFFu;

    constexpr BufferBarrier() noexcept = default;

    constexpr BufferBarrier(
        std::uint64_t buffer,
        PipelineStage srcStage,
        Access        srcAccess,
        PipelineStage dstStage,
        Access        dstAccess,
        std::uint64_t offset         = 0,
        std::uint64_t size           = kWholeSize,
        std::uint32_t srcQueueFamily = kQueueFamilyIgnored,
        std::uint32_t dstQueueFamily = kQueueFamilyIgnored
    ) noexcept:
        resource(buffer), offset(offset), size(size), stageBefore(srcStage), accessBefore(srcAccess), stageAfter(dstStage),
        accessAfter(dstAccess), srcQueueFamily(srcQueueFamily), dstQueueFamily(dstQueueFamily) {
    }

    // -------------------------------------------------------------------------
    // Semantic Intent Factories
    // -------------------------------------------------------------------------

    /// Transfer/clear write to compute shader read/write.
    [[nodiscard]] static constexpr auto
        TransferToCompute(std::uint64_t buffer, std::uint64_t offset = 0, std::uint64_t size = kWholeSize) noexcept -> BufferBarrier {
        return {
            buffer, PipelineStage::Transfer, Access::TransferWrite, PipelineStage::Compute, Access::ShaderRead | Access::ShaderWrite, offset, size,
        };
    }

    /// Compute shader read/write to subsequent transfer/clear write.
    [[nodiscard]] static constexpr auto
        ComputeToTransfer(std::uint64_t buffer, std::uint64_t offset = 0, std::uint64_t size = kWholeSize) noexcept -> BufferBarrier {
        return {
            buffer, PipelineStage::Compute, Access::ShaderRead | Access::ShaderWrite, PipelineStage::Transfer, Access::TransferWrite, offset, size,
        };
    }

    /// Compute write + indirect draw consumption to subsequent transfer/clear reset.
    [[nodiscard]] static constexpr auto
        IndirectToTransfer(std::uint64_t buffer, std::uint64_t offset = 0, std::uint64_t size = kWholeSize) noexcept -> BufferBarrier {
        return {
            buffer, PipelineStage::Compute | PipelineStage::Indirect, Access::ShaderWrite | Access::IndirectRead,
            PipelineStage::Transfer, Access::TransferWrite, offset, size,
        };
    }

    /// Compute culling / command generation write to indirect draw/dispatch argument read.
    [[nodiscard]] static constexpr auto
        ComputeToIndirect(std::uint64_t buffer, std::uint64_t offset = 0, std::uint64_t size = kWholeSize) noexcept -> BufferBarrier {
        return {buffer, PipelineStage::Compute, Access::ShaderWrite, PipelineStage::Indirect, Access::IndirectRead, offset, size};
    }

    /// Compute shader SSBO/BDA write to subsequent compute shader SSBO/BDA read.
    [[nodiscard]] static constexpr auto
        ComputeWriteToRead(std::uint64_t buffer, std::uint64_t offset = 0, std::uint64_t size = kWholeSize) noexcept -> BufferBarrier {
        return {buffer, PipelineStage::Compute, Access::ShaderWrite, PipelineStage::Compute, Access::ShaderRead, offset, size};
    }

    /// Serialize overlapping staged copy writes targeting the same destination buffer range.
    [[nodiscard]] static constexpr auto
        CopyWriteToWrite(std::uint64_t buffer, std::uint64_t offset = 0, std::uint64_t size = kWholeSize) noexcept -> BufferBarrier {
        return {buffer, PipelineStage::Copy, Access::TransferWrite, PipelineStage::Copy, Access::TransferWrite, offset, size};
    }

    /// Transfer/copy write to indirect draw/dispatch argument read.
    [[nodiscard]] static constexpr auto
        TransferToIndirect(std::uint64_t buffer, std::uint64_t offset = 0, std::uint64_t size = kWholeSize) noexcept -> BufferBarrier {
        return {buffer, PipelineStage::Transfer, Access::TransferWrite, PipelineStage::Indirect, Access::IndirectRead, offset, size};
    }

    /// Staged upload / transfer write to vertex attribute, index, and vertex shader BDA reads.
    [[nodiscard]] static constexpr auto
        TransferToVertex(std::uint64_t buffer, std::uint64_t offset = 0, std::uint64_t size = kWholeSize) noexcept -> BufferBarrier {
        return {
            buffer, PipelineStage::Transfer, Access::TransferWrite, PipelineStage::VertexInput | PipelineStage::Vertex,
            Access::VertexAttributeRead | Access::IndexRead | Access::ShaderRead, offset, size,
        };
    }

    /// Queue-family ownership release on the source queue (Vulkan-only).
    [[nodiscard]] static constexpr auto QueueRelease(
        std::uint64_t buffer,
        PipelineStage srcStage,
        Access        srcAccess,
        std::uint32_t srcQueueFamily,
        std::uint32_t dstQueueFamily,
        std::uint64_t offset = 0,
        std::uint64_t size   = kWholeSize
    ) noexcept -> BufferBarrier {
        return {buffer, srcStage, srcAccess, PipelineStage::None, Access::None, offset, size, srcQueueFamily, dstQueueFamily};
    }

    /// Queue-family ownership acquire on the destination queue (Vulkan-only).
    [[nodiscard]] static constexpr auto QueueAcquire(
        std::uint64_t buffer,
        PipelineStage dstStage,
        Access        dstAccess,
        std::uint32_t srcQueueFamily,
        std::uint32_t dstQueueFamily,
        std::uint64_t offset = 0,
        std::uint64_t size   = kWholeSize
    ) noexcept -> BufferBarrier {
        return {buffer, PipelineStage::None, Access::None, dstStage, dstAccess, offset, size, srcQueueFamily, dstQueueFamily};
    }

    // -------------------------------------------------------------------------
    // TDA factories from domain objects: a slice-like object (public
    // buffer/offset/size) or a buffer-like object (Handle()) states the
    // range; the caller never unpacks it.
    // -------------------------------------------------------------------------

    template <typename Slice>
        requires TemplatedDetail::BufferSliceDomain<Slice>
    [[nodiscard]] static constexpr auto TransferToCompute(const Slice& slice) noexcept -> BufferBarrier {
        return TransferToCompute(TemplatedDetail::SliceBufferWord(slice), TemplatedDetail::SliceOffset(slice), TemplatedDetail::SliceSize(slice));
    }

    template <typename Buffer>
        requires TemplatedDetail::BufferDomain<Buffer>
    [[nodiscard]] static constexpr auto
        TransferToCompute(const Buffer& buffer, std::uint64_t offset = 0, std::uint64_t size = kWholeSize) noexcept -> BufferBarrier {
        return TransferToCompute(TemplatedDetail::DomainBufferWord(buffer), offset, size);
    }

    template <typename Slice>
        requires TemplatedDetail::BufferSliceDomain<Slice>
    [[nodiscard]] static constexpr auto ComputeToTransfer(const Slice& slice) noexcept -> BufferBarrier {
        return ComputeToTransfer(TemplatedDetail::SliceBufferWord(slice), TemplatedDetail::SliceOffset(slice), TemplatedDetail::SliceSize(slice));
    }

    template <typename Buffer>
        requires TemplatedDetail::BufferDomain<Buffer>
    [[nodiscard]] static constexpr auto
        ComputeToTransfer(const Buffer& buffer, std::uint64_t offset = 0, std::uint64_t size = kWholeSize) noexcept -> BufferBarrier {
        return ComputeToTransfer(TemplatedDetail::DomainBufferWord(buffer), offset, size);
    }

    template <typename Slice>
        requires TemplatedDetail::BufferSliceDomain<Slice>
    [[nodiscard]] static constexpr auto IndirectToTransfer(const Slice& slice) noexcept -> BufferBarrier {
        return IndirectToTransfer(TemplatedDetail::SliceBufferWord(slice), TemplatedDetail::SliceOffset(slice), TemplatedDetail::SliceSize(slice));
    }

    template <typename Buffer>
        requires TemplatedDetail::BufferDomain<Buffer>
    [[nodiscard]] static constexpr auto
        IndirectToTransfer(const Buffer& buffer, std::uint64_t offset = 0, std::uint64_t size = kWholeSize) noexcept -> BufferBarrier {
        return IndirectToTransfer(TemplatedDetail::DomainBufferWord(buffer), offset, size);
    }

    template <typename Slice>
        requires TemplatedDetail::BufferSliceDomain<Slice>
    [[nodiscard]] static constexpr auto ComputeToIndirect(const Slice& slice) noexcept -> BufferBarrier {
        return ComputeToIndirect(TemplatedDetail::SliceBufferWord(slice), TemplatedDetail::SliceOffset(slice), TemplatedDetail::SliceSize(slice));
    }

    template <typename Buffer>
        requires TemplatedDetail::BufferDomain<Buffer>
    [[nodiscard]] static constexpr auto
        ComputeToIndirect(const Buffer& buffer, std::uint64_t offset = 0, std::uint64_t size = kWholeSize) noexcept -> BufferBarrier {
        return ComputeToIndirect(TemplatedDetail::DomainBufferWord(buffer), offset, size);
    }

    template <typename Slice>
        requires TemplatedDetail::BufferSliceDomain<Slice>
    [[nodiscard]] static constexpr auto ComputeWriteToRead(const Slice& slice) noexcept -> BufferBarrier {
        return ComputeWriteToRead(TemplatedDetail::SliceBufferWord(slice), TemplatedDetail::SliceOffset(slice), TemplatedDetail::SliceSize(slice));
    }

    template <typename Buffer>
        requires TemplatedDetail::BufferDomain<Buffer>
    [[nodiscard]] static constexpr auto
        ComputeWriteToRead(const Buffer& buffer, std::uint64_t offset = 0, std::uint64_t size = kWholeSize) noexcept -> BufferBarrier {
        return ComputeWriteToRead(TemplatedDetail::DomainBufferWord(buffer), offset, size);
    }

    template <typename Slice>
        requires TemplatedDetail::BufferSliceDomain<Slice>
    [[nodiscard]] static constexpr auto CopyWriteToWrite(const Slice& slice) noexcept -> BufferBarrier {
        return CopyWriteToWrite(TemplatedDetail::SliceBufferWord(slice), TemplatedDetail::SliceOffset(slice), TemplatedDetail::SliceSize(slice));
    }

    template <typename Buffer>
        requires TemplatedDetail::BufferDomain<Buffer>
    [[nodiscard]] static constexpr auto
        CopyWriteToWrite(const Buffer& buffer, std::uint64_t offset = 0, std::uint64_t size = kWholeSize) noexcept -> BufferBarrier {
        return CopyWriteToWrite(TemplatedDetail::DomainBufferWord(buffer), offset, size);
    }

    template <typename Slice>
        requires TemplatedDetail::BufferSliceDomain<Slice>
    [[nodiscard]] static constexpr auto TransferToIndirect(const Slice& slice) noexcept -> BufferBarrier {
        return TransferToIndirect(TemplatedDetail::SliceBufferWord(slice), TemplatedDetail::SliceOffset(slice), TemplatedDetail::SliceSize(slice));
    }

    template <typename Buffer>
        requires TemplatedDetail::BufferDomain<Buffer>
    [[nodiscard]] static constexpr auto
        TransferToIndirect(const Buffer& buffer, std::uint64_t offset = 0, std::uint64_t size = kWholeSize) noexcept -> BufferBarrier {
        return TransferToIndirect(TemplatedDetail::DomainBufferWord(buffer), offset, size);
    }

    template <typename Slice>
        requires TemplatedDetail::BufferSliceDomain<Slice>
    [[nodiscard]] static constexpr auto TransferToVertex(const Slice& slice) noexcept -> BufferBarrier {
        return TransferToVertex(TemplatedDetail::SliceBufferWord(slice), TemplatedDetail::SliceOffset(slice), TemplatedDetail::SliceSize(slice));
    }

    template <typename Buffer>
        requires TemplatedDetail::BufferDomain<Buffer>
    [[nodiscard]] static constexpr auto
        TransferToVertex(const Buffer& buffer, std::uint64_t offset = 0, std::uint64_t size = kWholeSize) noexcept -> BufferBarrier {
        return TransferToVertex(TemplatedDetail::DomainBufferWord(buffer), offset, size);
    }

    template <typename Slice>
        requires TemplatedDetail::BufferSliceDomain<Slice>
    [[nodiscard]] static constexpr auto QueueRelease(
        const Slice& slice, PipelineStage srcStage, Access srcAccess, std::uint32_t srcQueueFamily, std::uint32_t dstQueueFamily
    ) noexcept -> BufferBarrier {
        return QueueRelease(
            TemplatedDetail::SliceBufferWord(slice), srcStage, srcAccess, srcQueueFamily, dstQueueFamily, TemplatedDetail::SliceOffset(slice), TemplatedDetail::SliceSize(slice)
        );
    }

    template <typename Buffer>
        requires TemplatedDetail::BufferDomain<Buffer>
    [[nodiscard]] static constexpr auto QueueRelease(
        const Buffer& buffer,
        PipelineStage srcStage,
        Access        srcAccess,
        std::uint32_t srcQueueFamily,
        std::uint32_t dstQueueFamily,
        std::uint64_t offset = 0,
        std::uint64_t size   = kWholeSize
    ) noexcept -> BufferBarrier {
        return QueueRelease(TemplatedDetail::DomainBufferWord(buffer), srcStage, srcAccess, srcQueueFamily, dstQueueFamily, offset, size);
    }

    template <typename Slice>
        requires TemplatedDetail::BufferSliceDomain<Slice>
    [[nodiscard]] static constexpr auto QueueAcquire(
        const Slice& slice, PipelineStage dstStage, Access dstAccess, std::uint32_t srcQueueFamily, std::uint32_t dstQueueFamily
    ) noexcept -> BufferBarrier {
        return QueueAcquire(
            TemplatedDetail::SliceBufferWord(slice), dstStage, dstAccess, srcQueueFamily, dstQueueFamily, TemplatedDetail::SliceOffset(slice), TemplatedDetail::SliceSize(slice)
        );
    }

    template <typename Buffer>
        requires TemplatedDetail::BufferDomain<Buffer>
    [[nodiscard]] static constexpr auto QueueAcquire(
        const Buffer& buffer,
        PipelineStage dstStage,
        Access        dstAccess,
        std::uint32_t srcQueueFamily,
        std::uint32_t dstQueueFamily,
        std::uint64_t offset = 0,
        std::uint64_t size   = kWholeSize
    ) noexcept -> BufferBarrier {
        return QueueAcquire(TemplatedDetail::DomainBufferWord(buffer), dstStage, dstAccess, srcQueueFamily, dstQueueFamily, offset, size);
    }

    // -------------------------------------------------------------------------
    // Execution ("Tell the barrier to record"): defined by the backend TDA
    // header for its command type (<vkd3d/VulkanBarriers.hpp> for
    // VkCommandBuffer).
    // -------------------------------------------------------------------------
    template <typename CommandBuffer>
    void Record(CommandBuffer cmd) const noexcept;

    template <typename CommandBuffer, std::size_t N>
    static void Record(CommandBuffer cmd, const std::array<BufferBarrier, N>& barriers) noexcept;

    template <typename CommandBuffer>
    static void Record(CommandBuffer cmd, std::span<const BufferBarrier> barriers) noexcept;

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool { return resource != 0 && size != 0; }

    friend constexpr auto operator==(const BufferBarrier&, const BufferBarrier&) noexcept -> bool = default;

    std::uint64_t resource       = 0;
    std::uint64_t offset         = 0;
    std::uint64_t size           = kWholeSize;
    PipelineStage stageBefore    = PipelineStage::None;
    Access        accessBefore   = Access::None;
    PipelineStage stageAfter     = PipelineStage::None;
    Access        accessAfter    = Access::None;
    std::uint32_t srcQueueFamily = kQueueFamilyIgnored;
    std::uint32_t dstQueueFamily = kQueueFamilyIgnored;
};

static_assert(sizeof(BufferBarrier) == 48, "BufferBarrier must be exactly 48 bytes.");
static_assert(std::is_trivially_copyable_v<BufferBarrier>);

// -----------------------------------------------------------------------------
// ImageBarrier (48 bytes)
// -----------------------------------------------------------------------------

/// Subresource selection for the ImageBarrier factories, in neutral terms.
/// mipLevelCount == ImageBarrier::kRemainingMips selects all remaining mip
/// levels; arrayLayerCount == ImageBarrier::kRemainingLayers all layers.
struct ImageSubresourceRange {
    ImageAspect   aspect          = ImageAspect::Color;
    std::uint32_t baseMipLevel    = 0;
    std::uint32_t mipLevelCount   = 0xFFFFu;
    std::uint32_t baseArrayLayer  = 0;
    std::uint32_t arrayLayerCount = 0xFFFFFFFFu;
};

namespace TemplatedDetail {

/// A domain range is either already neutral (aspect field) or a backend
/// range with an aspectMask plus base/count fields; both collapse to the
/// neutral selection the barrier stores.
template <typename Range>
[[nodiscard]] constexpr auto NeutralRange(const Range& range) noexcept -> ImageSubresourceRange {
    if constexpr (requires { range.aspect; }) {
        return range;
    } else {
        return {
            .aspect          = AspectFromMask(range.aspectMask),
            .baseMipLevel    = range.baseMipLevel,
            .mipLevelCount   = range.levelCount,
            .baseArrayLayer  = range.baseArrayLayer,
            .arrayLayerCount = range.layerCount,
        };
    }
}

template <typename View>
[[nodiscard]] constexpr auto ImageWord(const View& view) noexcept -> std::uint64_t {
    if constexpr (requires { view.Handle(); }) {
        return ResourceWord(view.Handle());
    } else {
        return ImageWord(view.View());
    }
}

template <typename View>
[[nodiscard]] constexpr auto ImageRange(const View& view) noexcept -> ImageSubresourceRange {
    if constexpr (requires { view.SubresourceRange(); }) {
        return NeutralRange(view.SubresourceRange());
    } else {
        return ImageRange(view.View());
    }
}

} // namespace TemplatedDetail

// Layout transition + hazard for one image subresource range. The resource
// word is the backend's image identity -- on Vulkan, the VkImage value.
class ImageBarrier {
  public:
    static constexpr std::uint16_t kRemainingMips   = 0xFFFFu;
    static constexpr std::uint32_t kRemainingLayers = 0xFFFF'FFFFu;

    constexpr ImageBarrier() noexcept = default;

    constexpr ImageBarrier(
        std::uint64_t        image,
        ImageSubresourceRange range,
        ImageLayout          oldLayout,
        ImageLayout          newLayout,
        PipelineStage        srcStage,
        Access               srcAccess,
        PipelineStage        dstStage,
        Access               dstAccess
    ) noexcept:
        resource(image), stageBefore(srcStage), accessBefore(srcAccess), stageAfter(dstStage), accessAfter(dstAccess), layoutBefore(oldLayout),
        layoutAfter(newLayout), aspect(range.aspect), baseArrayLayer(range.baseArrayLayer), arrayLayerCount(range.arrayLayerCount),
        baseMipLevel(static_cast<std::uint16_t>(range.baseMipLevel)),
        mipLevelCount(range.mipLevelCount >= kRemainingMips ? kRemainingMips : static_cast<std::uint16_t>(range.mipLevelCount)) {
    }

    // -------------------------------------------------------------------------
    // Semantic Intent Factories
    // -------------------------------------------------------------------------

    /// Automatic layout transition; stages/accesses deduced from the layouts.
    template <ImageLayout OldLayout, ImageLayout NewLayout>
    [[nodiscard]] static constexpr auto Transition(std::uint64_t image, ImageSubresourceRange range = {}) noexcept -> ImageBarrier {
        using Src = LayoutTraits<OldLayout>;
        using Dst = LayoutTraits<NewLayout>;
        return {image, range, OldLayout, NewLayout, Src::kStage, Src::kAccess, Dst::kStage, Dst::kAccess};
    }

    /// Runtime layout transition deriving conservative sync masks from oldLayout / newLayout.
    [[nodiscard]] static constexpr auto
        Transition(std::uint64_t image, ImageSubresourceRange range, ImageLayout oldLayout, ImageLayout newLayout) noexcept -> ImageBarrier {
        const LayoutSyncInfo src = GetLayoutSyncInfo(oldLayout, true);
        const LayoutSyncInfo dst = GetLayoutSyncInfo(newLayout, false);
        return {image, range, oldLayout, newLayout, src.stage, src.access, dst.stage, dst.access};
    }

    /// Transition with explicit stage/access override (e.g. frame-graph state tables).
    [[nodiscard]] static constexpr auto Transition(
        std::uint64_t        image,
        ImageSubresourceRange range,
        ImageLayout          oldLayout,
        ImageLayout          newLayout,
        PipelineStage        srcStage,
        Access               srcAccess,
        PipelineStage        dstStage,
        Access               dstAccess
    ) noexcept -> ImageBarrier {
        return {image, range, oldLayout, newLayout, srcStage, srcAccess, dstStage, dstAccess};
    }

    /// Discard existing contents (Undefined -> TargetLayout).
    template <ImageLayout TargetLayout>
    [[nodiscard]] static constexpr auto DiscardTo(std::uint64_t image, ImageSubresourceRange range = {}) noexcept -> ImageBarrier {
        return Transition<ImageLayout::Undefined, TargetLayout>(image, range);
    }

    /// Presentation handoff: currentLayout -> PresentSrc, callable index 0
    /// of the image (backends with a present queue consume the same value).
    [[nodiscard]] static constexpr auto PreparePresent(std::uint64_t image, ImageLayout currentLayout) noexcept -> ImageBarrier {
        return {
            image,
            ImageSubresourceRange {.aspect = ImageAspect::Color, .baseMipLevel = 0, .mipLevelCount = 1, .baseArrayLayer = 0, .arrayLayerCount = 1},
            currentLayout, ImageLayout::PresentSrc, PipelineStage::AllCommands,
            Access::ColorWrite | Access::ShaderWrite | Access::TransferWrite, PipelineStage::BottomOfPipe, Access::None,
        };
    }

    // -------------------------------------------------------------------------
    // TDA factories from domain objects: hand over the view (or slice); the
    // barrier asks it for handle + subresource range. Callers state only the
    // layout intent.
    // -------------------------------------------------------------------------

    /// Automatic layout transition for any image view/slice domain object.
    template <ImageLayout OldLayout, ImageLayout NewLayout, typename View>
        requires TemplatedDetail::ImageDomain<View>
    [[nodiscard]] static constexpr auto Transition(const View& view) noexcept -> ImageBarrier {
        return Transition<OldLayout, NewLayout>(TemplatedDetail::ImageWord(view), TemplatedDetail::ImageRange(view));
    }

    /// Runtime layout transition for any image view/slice domain object.
    template <typename View>
        requires TemplatedDetail::ImageDomain<View>
    [[nodiscard]] static constexpr auto Transition(const View& view, ImageLayout oldLayout, ImageLayout newLayout) noexcept -> ImageBarrier {
        return Transition(TemplatedDetail::ImageWord(view), TemplatedDetail::ImageRange(view), oldLayout, newLayout);
    }

    /// Explicit stage/access transition for any image view/slice domain object.
    template <typename View>
        requires TemplatedDetail::ImageDomain<View>
    [[nodiscard]] static constexpr auto Transition(
        const View& view, ImageLayout oldLayout, ImageLayout newLayout, PipelineStage srcStage, Access srcAccess, PipelineStage dstStage, Access dstAccess
    ) noexcept -> ImageBarrier {
        return Transition(TemplatedDetail::ImageWord(view), TemplatedDetail::ImageRange(view), oldLayout, newLayout, srcStage, srcAccess, dstStage, dstAccess);
    }

    /// Discard existing contents (Undefined -> TargetLayout) for a domain object.
    template <ImageLayout TargetLayout, typename View>
        requires TemplatedDetail::ImageDomain<View>
    [[nodiscard]] static constexpr auto DiscardTo(const View& view) noexcept -> ImageBarrier {
        return Transition<ImageLayout::Undefined, TargetLayout>(view);
    }

    // -------------------------------------------------------------------------
    // Execution ("Tell the barrier to record"): defined by the backend TDA
    // header for its command type (<vkd3d/VulkanBarriers.hpp> for
    // VkCommandBuffer).
    // -------------------------------------------------------------------------
    template <typename CommandBuffer>
    void Record(CommandBuffer cmd) const noexcept;

    template <typename CommandBuffer, std::size_t N>
    static void Record(CommandBuffer cmd, const std::array<ImageBarrier, N>& barriers) noexcept;

    template <typename CommandBuffer>
    static void Record(CommandBuffer cmd, std::span<const ImageBarrier> barriers) noexcept;

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool { return resource != 0; }

    std::uint64_t resource        = 0;                      // 8 bytes
    PipelineStage stageBefore     = PipelineStage::None;    // 4 bytes
    Access        accessBefore    = Access::None;           // 4 bytes
    PipelineStage stageAfter      = PipelineStage::None;    // 4 bytes
    Access        accessAfter     = Access::None;           // 4 bytes
    ImageLayout   layoutBefore    = ImageLayout::Undefined; // 4 bytes
    ImageLayout   layoutAfter     = ImageLayout::Undefined; // 4 bytes
    ImageAspect   aspect          = ImageAspect::Color;     // 4 bytes
    std::uint32_t baseArrayLayer  = 0;                      // 4 bytes
    std::uint32_t arrayLayerCount = kRemainingLayers;       // 4 bytes
    std::uint16_t baseMipLevel    = 0;                      // 2 bytes
    std::uint16_t mipLevelCount   = kRemainingMips;         // 2 bytes
};

static_assert(sizeof(ImageBarrier) == 48, "ImageBarrier must be exactly 48 bytes.");
static_assert(std::is_trivially_copyable_v<ImageBarrier>);

} // namespace ZHLN::VkD3D
