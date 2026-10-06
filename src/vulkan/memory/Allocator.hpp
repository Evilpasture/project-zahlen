// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other render headers."
#endif

#include <Zahlen/Threading/Mutex.hpp>

// VMA forward declarations. <vk_mem_alloc.h> is pulled in only by
// Allocator.cpp and VmaImplementation.cpp; every other TU sees VMA as opaque
// pointer handles, so VMA headers never leak out of src/vulkan/memory/.
struct VmaAllocator_T;
struct VmaAllocation_T;
using VmaAllocator  = struct VmaAllocator_T*;
using VmaAllocation = struct VmaAllocation_T*;

namespace ZHLN::Vk {

class Context;
class Buffer;
class Image;

struct DeferredDeletionEntry {
    enum class Type : uint8_t { Buffer, Image, AccelerationStructure, Pipeline };
    Type          type;
    VmaAllocation allocation = nullptr;
    VkDevice      device     = VK_NULL_HANDLE;
    union {
        VkBuffer                   buffer;
        VkImage                    image;
        VkAccelerationStructureKHR accelerationStructure;
        VkPipeline                 pipeline;
    };
};

class Allocator;

class DeletionQueue {
  public:
    DeletionQueue() = default;
    ~DeletionQueue();

    DeletionQueue(const DeletionQueue&)            = delete;
    DeletionQueue& operator=(const DeletionQueue&) = delete;

    void Init(Allocator& allocator) noexcept {
        _allocator = &allocator;
    }
    // Consumes the handle. The caller must retire associated views before an
    // image, or acceleration structures before their backing buffers.
    void Enqueue(Buffer&& buffer) noexcept;
    void Enqueue(Image&& image) noexcept;
    void EnqueueAccelerationStructure(VkDevice device, AccelerationStructure&& handle) noexcept;
    void EnqueuePipeline(VkDevice device, VkPipeline pipeline) noexcept;
    // Called only after the frame slot's GPU fence has been waited on.
    void BeginFrame(uint32_t frameIndex) noexcept;
    // The caller must wait for the GPU to be idle before flushing every slot.
    void Drain() noexcept;

  private:
    void CleanupQueue(std::vector<DeferredDeletionEntry>& queue) noexcept;

    Allocator* _allocator = nullptr; // Must outlive the queue and its final drain.
    // Mutex is trivially default-constructible in release builds: a cold
    // renderer can enqueue during teardown without ever calling BeginFrame.
    ZHLN::Mutex                                                     _mutex {};
    std::array<std::vector<DeferredDeletionEntry>, kFramesInFlight> _queues {};
    uint32_t                                                        _currentFrameIndex = 0;
};

class Allocator {
  public:
    Allocator() = default;
    ~Allocator() noexcept;

    Allocator(const Allocator&)                    = delete;
    auto operator=(const Allocator&) -> Allocator& = delete;

    Allocator(Allocator&& other) noexcept;
    auto operator=(Allocator&& other) noexcept -> Allocator&;

    [[nodiscard]] auto Init(VkInstance instance, VkPhysicalDevice physical, VkDevice device) noexcept -> std::expected<void, ZHLN::ErrorCode>;
    [[nodiscard]] auto Init(const Context& ctx) noexcept -> std::expected<void, ZHLN::ErrorCode>;

    // Synchronous destruction: only use once GPU use has finished. Explicitly
    // consumes and invalidates the handle; neither Buffer nor Image has a
    // destructor that consults ambient state.
    void DestroyBuffer(Buffer& buffer) const noexcept;
    void DestroyImage(Image& image) const noexcept;

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _handle != nullptr;
    }
    explicit operator bool() const noexcept {
        return Valid();
    }

  private:
    // Only Buffer/Image/ImageBuilder/StagingRingBuffer/DeletionQueue are
    // permitted to reach the raw VMA handle. Public APIs accept Allocator& so
    // callers in src/render never see VmaAllocator.
    friend class Buffer;
    friend class Image;
    friend class ImageBuilder;
    friend class StagingRingBuffer;
    friend class DeletionQueue;

    [[nodiscard]] auto Handle() const noexcept -> VmaAllocator {
        return _handle;
    }

    // Friend-only statics used by the cleanup paths.
    static void DestroyBuffer(VmaAllocator allocator, Buffer& buffer) noexcept;
    static void DestroyImage(VmaAllocator allocator, Image& image) noexcept;

    VmaAllocator _handle = nullptr;
};

// Engine-owned memory classification. Values are mapped to the underlying
// allocator's memory-usage enum via an exhaustive switch inside Allocator.cpp
// (the only TU that includes vk_mem_alloc.h). Adding an enumerator here forces
// a compile error there until it is handled.
enum class MemoryUsage : uint8_t {
    GPUOnly,
    CPUOnly,
    CPUToGPU,
    GPUToCPU,
};

// NOLINTNEXTLINE(performance-enum-size)
enum class BufferUsage : VkBufferUsageFlags {
    None                            = 0,
    TransferSrc                     = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    TransferDst                     = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    Uniform                         = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    Storage                         = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    Index                           = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
    Vertex                          = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
    Indirect                        = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
    ShaderDeviceAddress             = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    AccelerationStructureStorage    = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
    AccelerationStructureBuildInput = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
    DescriptorHeap                  = VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT,
};

// NOLINTNEXTLINE(performance-enum-size)
enum class ImageUsage : VkImageUsageFlags {
    None                   = 0,
    TransferSrc            = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    TransferDst            = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    Sampled                = VK_IMAGE_USAGE_SAMPLED_BIT,
    Storage                = VK_IMAGE_USAGE_STORAGE_BIT,
    ColorAttachment        = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
    DepthStencilAttachment = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
    TransientAttachment    = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
    InputAttachment        = VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
};

[[nodiscard]] constexpr auto ToVk(BufferUsage usage) noexcept -> VkBufferUsageFlags {
    return static_cast<VkBufferUsageFlags>(usage);
}
[[nodiscard]] constexpr auto ToVk(ImageUsage usage) noexcept -> VkImageUsageFlags {
    return static_cast<VkImageUsageFlags>(usage);
}

[[nodiscard]] constexpr auto operator|(BufferUsage a, BufferUsage b) noexcept -> BufferUsage {
    return static_cast<BufferUsage>(ToVk(a) | ToVk(b));
}
constexpr auto operator|=(BufferUsage& a, BufferUsage b) noexcept -> BufferUsage& {
    a = a | b;
    return a;
}
[[nodiscard]] constexpr auto operator&(BufferUsage a, BufferUsage b) noexcept -> BufferUsage {
    return static_cast<BufferUsage>(ToVk(a) & ToVk(b));
}
[[nodiscard]] constexpr auto Has(BufferUsage flags, BufferUsage bits) noexcept -> bool {
    return (ToVk(flags) & ToVk(bits)) != 0;
}

[[nodiscard]] constexpr auto operator|(ImageUsage a, ImageUsage b) noexcept -> ImageUsage {
    return static_cast<ImageUsage>(ToVk(a) | ToVk(b));
}
constexpr auto operator|=(ImageUsage& a, ImageUsage b) noexcept -> ImageUsage& {
    a = a | b;
    return a;
}
[[nodiscard]] constexpr auto operator&(ImageUsage a, ImageUsage b) noexcept -> ImageUsage {
    return static_cast<ImageUsage>(ToVk(a) & ToVk(b));
}
[[nodiscard]] constexpr auto Has(ImageUsage flags, ImageUsage bits) noexcept -> bool {
    return (ToVk(flags) & ToVk(bits)) != 0;
}

class Buffer {
  public:
    Buffer()           = default;
    ~Buffer() noexcept = default; // Non-destroying: owner must choose DestroyBuffer or Enqueue.

    Buffer(const Buffer&)                    = delete;
    auto operator=(const Buffer&) -> Buffer& = delete;

    Buffer(Buffer&& other) noexcept;
    auto operator=(Buffer&& other) noexcept -> Buffer&;

    [[nodiscard]] static auto Create(Allocator& allocator, size_t size, BufferUsage usage, MemoryUsage memUsage) noexcept -> std::expected<Buffer, ErrorCode>;

    [[nodiscard]] static auto Create(Allocator& allocator, size_t size, BufferUsage usage, MemoryUsage memUsage, VkDeviceSize minAlignment) noexcept
        -> std::expected<Buffer, ErrorCode>;

    [[nodiscard]] static auto Create(
        Allocator&                allocator,
        size_t                    size,
        BufferUsage               usage,
        MemoryUsage               memUsage,
        VkDeviceSize              minAlignment,
        VkSharingMode             sharingMode,
        std::span<const uint32_t> queueFamilyIndices
    ) noexcept -> std::expected<Buffer, ErrorCode>;

    void Flush(Allocator& allocator, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept;

    // A window onto host-visible buffer memory. CPU-writable buffers are created with
    // VMA_ALLOCATION_CREATE_MAPPED_BIT, so VMA maps them once and holds that mapping for
    // the allocation's lifetime: this type never needs vmaMapMemory() or vmaUnmapMemory(),
    // it only flushes what was written when it leaves scope, which is all non-coherent
    // memory asks for.
    //
    // The default-constructed region is empty -- "nothing mapped" -- which is what the
    // long-lived holders reset to. A region handed to a caller by Map() is never empty: a
    // buffer with no persistent mapping is reported as an ErrorCode instead, so Data() is
    // never null where a caller uses it.
    struct MappedRegion {
        MappedRegion() = default;
        ~MappedRegion() noexcept;

        MappedRegion(const MappedRegion&)                    = delete;
        auto operator=(const MappedRegion&) -> MappedRegion& = delete;

        MappedRegion(MappedRegion&& other) noexcept;
        auto operator=(MappedRegion&& other) noexcept -> MappedRegion&;

        [[nodiscard]] auto Data() const noexcept -> void* {
            return _ptr;
        }
        // The size the buffer was created with, in bytes: what AsSpan() clamps to.
        [[nodiscard]] auto Size() const noexcept -> size_t {
            return _size;
        }
        template <typename T>
        [[nodiscard]] auto As() const noexcept -> T* {
            return static_cast<T*>(_ptr);
        }
        // count == 0 means "all of it".
        template <typename T>
        [[nodiscard]] auto AsSpan(size_t count = 0) const noexcept -> std::span<T> {
            const size_t available = _size / sizeof(T);
            return std::span<T>(static_cast<T*>(_ptr), count == 0 ? available : std::min(count, available));
        }

      private:
        friend class Buffer;
        MappedRegion(Allocator& alloc, VmaAllocation allocation, void* ptr, size_t size) noexcept;
        void          Cleanup() noexcept;
        void*         _ptr        = nullptr;
        size_t        _size       = 0;
        VmaAllocator  _allocator  = nullptr;
        VmaAllocation _allocation = nullptr;
    };

    // A buffer only has memory to map once Create() succeeded, and only host-visible
    // memory carries a persistent mapping: those are the two ways Map() fails. Both
    // travel as an ErrorCode instead of a null pointer a caller could take for a valid
    // empty read.
    [[nodiscard]] auto Map(Allocator& allocator) noexcept -> std::expected<MappedRegion, ErrorCode>;
    [[nodiscard]] auto Handle() const noexcept -> VkBuffer {
        return _handle;
    }
    [[nodiscard]] auto Size() const noexcept -> size_t {
        return _requestedSize;
    }
    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _handle != VK_NULL_HANDLE;
    }
    explicit operator bool() const noexcept {
        return Valid();
    }

    // Transfers the VMA allocation pair out, leaving this handle empty.
    [[nodiscard]] auto Release() noexcept -> std::pair<VkBuffer, VmaAllocation>;

  private:
    VkBuffer      _handle        = VK_NULL_HANDLE;
    VmaAllocation _allocation    = nullptr;
    void*         _mappedData    = nullptr; // pre-mapped pointer for CPU-visible allocations
    VkDeviceSize  _requestedSize = 0;
};

inline BufferSlice::BufferSlice(const Buffer& b, VkDeviceAddress base) noexcept: BufferSlice(b.Handle(), base, 0, static_cast<VkDeviceSize>(b.Size())) {
}

// Caller owns the returned staging buffer and must DestroyBuffer it after the
// recorded copy has completed (or enqueue it in the frame deletion queue).
[[nodiscard]] auto UploadToBuffer(Allocator& allocator, VkCommandBuffer cmd, Buffer& dst, const void* data, size_t size) noexcept -> Buffer;

class Image {
  public:
    Image()                                = default;
    ~Image() noexcept                      = default; // Non-destroying.
    Image(const Image&)                    = delete;
    auto operator=(const Image&) -> Image& = delete;
    Image(Image&& other) noexcept;
    auto operator=(Image&& other) noexcept -> Image&;

    [[nodiscard]] static auto Create(Allocator& allocator, const VkImageCreateInfo& info, MemoryUsage memUsage) -> std::expected<Image, ErrorCode>;

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _handle != VK_NULL_HANDLE;
    }
    explicit operator bool() const noexcept {
        return Valid();
    }
    [[nodiscard]] auto Handle() const noexcept -> VkImage {
        return _handle;
    }
    [[nodiscard]] auto Release() noexcept -> std::pair<VkImage, VmaAllocation>;

  private:
    VkImage       _handle     = VK_NULL_HANDLE;
    VmaAllocation _allocation = nullptr;
};

class ImageBuilder {
  public:
    ImageBuilder() noexcept;

    auto Type(VkImageType type) noexcept -> ImageBuilder&;
    auto Format(VkFormat format) noexcept -> ImageBuilder&;
    auto Dimensions(uint32_t width, uint32_t height, uint32_t depth = 1) noexcept -> ImageBuilder&;
    auto Mips(uint32_t levels) noexcept -> ImageBuilder&;
    auto Layers(uint32_t layers) noexcept -> ImageBuilder&;
    auto Samples(VkSampleCountFlagBits samples) noexcept -> ImageBuilder&;
    auto Tiling(VkImageTiling tiling) noexcept -> ImageBuilder&;
    auto Usage(ImageUsage usage) noexcept -> ImageBuilder&;
    auto SharingMode(VkSharingMode mode) noexcept -> ImageBuilder&;
    auto Flags(VkImageCreateFlags flags) noexcept -> ImageBuilder&;

    auto Texture2D(uint32_t width, uint32_t height, VkFormat format, ImageUsage usage, uint32_t mips = 1) noexcept -> ImageBuilder&;
    auto TextureCube(uint32_t size, VkFormat format, ImageUsage usage, uint32_t mips = 1) noexcept -> ImageBuilder&;

    [[nodiscard]] auto Build(Allocator& allocator, MemoryUsage memUsage = MemoryUsage::GPUOnly) const noexcept -> std::expected<Image, ErrorCode>;

  private:
    VkImageCreateInfo _info {};
};

template <typename T = uint32_t>
void FillBuffer(VkCommandBuffer cmd, const Buffer& buffer, VkDeviceSize offset = 0, T data = 0) {
    static_assert(sizeof(T) % 4 == 0, "Type must be 4-byte aligned for vkCmdFillBuffer");

    vkCmdFillBuffer(cmd, buffer.Handle(), offset, VK_WHOLE_SIZE, *reinterpret_cast<const uint32_t*>(&data));
}

inline void CopyBuffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, VkDeviceSize size, VkDeviceSize srcOffset = 0, VkDeviceSize dstOffset = 0) {
    const VkBufferCopy2 region {
        .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
        .srcOffset = srcOffset,
        .dstOffset = dstOffset,
        .size = size,
    };
    const VkCopyBufferInfo2 copyInfo {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
        .srcBuffer = src,
        .dstBuffer = dst,
        .regionCount = 1,
        .pRegions = &region,
    };
    vkCmdCopyBuffer2(cmd, &copyInfo);
}

inline void CopyBuffer(VkCommandBuffer cmd, const Buffer& src, const Buffer& dst, VkDeviceSize size, VkDeviceSize srcOffset = 0, VkDeviceSize dstOffset = 0) {
    CopyBuffer(cmd, src.Handle(), dst.Handle(), size, srcOffset, dstOffset);
}

inline void CopyBuffer(VkCommandBuffer cmd, BufferSlice src, BufferSlice dst) {
    if (!src.Valid() || !dst.Valid()) {
        return;
    }
    const VkDeviceSize bytes = std::min(src.Size(), dst.Size());
    if (bytes != 0) {
        CopyBuffer(cmd, src.buffer, dst.buffer, bytes, src.offset, dst.offset);
    }
}

inline void BufferBarrier(
    VkCommandBuffer cmd,
    const Buffer&   buffer,
    BarrierStage    srcStage,
    BarrierAccess   srcAccess,
    BarrierStage    dstStage,
    BarrierAccess   dstAccess
) noexcept {
    BufferBarrier(cmd, buffer.Handle(), srcStage, srcAccess, dstStage, dstAccess);
}

class StagingRingBuffer {
  public:
    struct Allocation {
        BufferSlice slice {};
        void*       mappedData    = nullptr;
        uint64_t    timelineValue = 0;
    };

    StagingRingBuffer() = default;
    ~StagingRingBuffer() noexcept {
        Cleanup();
    }

    StagingRingBuffer(const StagingRingBuffer&)                    = delete;
    auto operator=(const StagingRingBuffer&) -> StagingRingBuffer& = delete;

    StagingRingBuffer(StagingRingBuffer&& other) noexcept;
    auto operator=(StagingRingBuffer&& other) noexcept -> StagingRingBuffer&;

    [[nodiscard]] auto Init(Allocator& allocator, VkDevice device, VkQueue queue, uint32_t queueFamily, VkDeviceSize capacity) noexcept
        -> std::expected<void, ZHLN::ErrorCode>;
    void Cleanup() noexcept;

    [[nodiscard]] auto Allocate(VkDeviceSize size, VkDeviceSize alignment = 4) noexcept -> Allocation;
    auto               Submit(ExecutableCommands cmds, VkFence fence = VK_NULL_HANDLE) noexcept -> uint64_t;
    void               Recycle() noexcept;

    void RetirePool(VkCommandPool pool, uint64_t timelineValue) noexcept;

    [[nodiscard]] auto GetSemaphore() const noexcept -> VkSemaphore {
        return _timelineSemaphore.Get();
    }
    [[nodiscard]] auto GetCurrentValue() const noexcept -> uint64_t {
        return _timelineValue;
    }
    [[nodiscard]] auto GetQueueFamily() const noexcept -> uint32_t {
        return _queueFamily;
    }
    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _timelineSemaphore.Valid();
    }

  private:
    Allocator* _allocator   = nullptr;
    VkDevice   _device      = VK_NULL_HANDLE;
    VkQueue    _queue       = VK_NULL_HANDLE;
    uint32_t   _queueFamily = 0xFFFFFFFF;

    Buffer               _stagingBuffer;
    Buffer::MappedRegion _mappedRegion;
    void*                _mappedPtr = nullptr;
    VkDeviceSize         _capacity  = 0;

    VkDeviceSize _head = 0;
    VkDeviceSize _tail = 0;

    Semaphore _timelineSemaphore;
    uint64_t  _timelineValue = 0;

    struct ActiveAllocation {
        VkDeviceSize offset;
        VkDeviceSize size;
        uint64_t     timelineValue;
    };
    std::vector<ActiveAllocation> _activeAllocations;

    struct RetiredPool {
        VkCommandPool pool;
        uint64_t      timelineValue;
    };
    std::vector<RetiredPool> _retiredPools;
};

inline void CopyRingBuffer(VkCommandBuffer cmd, StagingRingBuffer::Allocation stagingAlloc, const Vk::Buffer& buffer) {
    CopyBuffer(cmd, stagingAlloc.slice, BufferSlice {buffer});
}

} // namespace ZHLN::Vk
