#pragma once
#include "RenderCore.h"
#include <vk_mem_alloc.h>
#include <cstring>

namespace ZHLN::Vk {

// ============================================================================
// Allocator RAII
// ============================================================================

class Allocator {
  public:
    Allocator() = default;
    ~Allocator() noexcept { if (_handle) vmaDestroyAllocator(_handle); }

    Allocator(const Allocator&)            = delete;
    Allocator& operator=(const Allocator&) = delete;

    Allocator(Allocator&& other) noexcept
        : _handle(std::exchange(other._handle, nullptr)) {}

    Allocator& operator=(Allocator&& other) noexcept {
        if (this != &other) {
            if (_handle) vmaDestroyAllocator(_handle);
            _handle = std::exchange(other._handle, nullptr);
        }
        return *this;
    }

    [[nodiscard]] bool Init(VkInstance instance,
                            VkPhysicalDevice physical,
                            VkDevice device) noexcept {
        VmaAllocatorCreateInfo info = {
            .flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
            .physicalDevice   = physical,
            .device           = device,
            .instance         = instance,
            .vulkanApiVersion = VK_API_VERSION_1_3,
        };
        return vmaCreateAllocator(&info, &_handle) == VK_SUCCESS;
    }

    [[nodiscard]] VmaAllocator Get()   const noexcept { return _handle; }
    [[nodiscard]] bool         Valid() const noexcept { return _handle != nullptr; }
    explicit operator bool()           const noexcept { return Valid(); }

  private:
    VmaAllocator _handle = nullptr;
};

// ============================================================================
// Buffer RAII
// ============================================================================

class Buffer {
  public:
    Buffer() = default;

    ~Buffer() noexcept {
        if (_handle != VK_NULL_HANDLE)
            vmaDestroyBuffer(_allocator, _handle, _allocation);
    }

    Buffer(const Buffer&)            = delete;
    Buffer& operator=(const Buffer&) = delete;

    Buffer(Buffer&& other) noexcept
        : _allocator (other._allocator)
        , _handle    (std::exchange(other._handle,     VK_NULL_HANDLE))
        , _allocation(std::exchange(other._allocation, nullptr))
        , _info      (other._info) {}

    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            if (_handle != VK_NULL_HANDLE)
                vmaDestroyBuffer(_allocator, _handle, _allocation);
            _allocator  = other._allocator;
            _handle     = std::exchange(other._handle,     VK_NULL_HANDLE);
            _allocation = std::exchange(other._allocation, nullptr);
            _info       = other._info;
        }
        return *this;
    }

    [[nodiscard]] static Buffer Create(VmaAllocator       allocator,
                                       size_t             size,
                                       VkBufferUsageFlags usage,
                                       VmaMemoryUsage     mem_usage) noexcept {
        Buffer b;
        b._allocator = allocator;

        VkBufferCreateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size  = size,
            .usage = usage,
        };
        VmaAllocationCreateInfo alloc_info = { .usage = mem_usage };

        if (vmaCreateBuffer(allocator, &buffer_info, &alloc_info,
                            &b._handle, &b._allocation, &b._info) != VK_SUCCESS)
            return {};

        return b;
    }

    // --- Mapped Region ---

    struct MappedRegion {
        MappedRegion(VmaAllocator alloc, VmaAllocation allocation, void* ptr) noexcept
            : _alloc(alloc), _allocation(allocation), data(ptr) {}

        ~MappedRegion() noexcept { vmaUnmapMemory(_alloc, _allocation); }

        MappedRegion(const MappedRegion&)            = delete;
        MappedRegion& operator=(const MappedRegion&) = delete;

        template <typename T>
        T* As() noexcept { return static_cast<T*>(data); }

        void* data = nullptr;

      private:
        VmaAllocator  _alloc;
        VmaAllocation _allocation;
    };

    [[nodiscard]] MappedRegion Map() noexcept {
        void* ptr = nullptr;
        vmaMapMemory(_allocator, _allocation, &ptr);
        return { _allocator, _allocation, ptr };
    }

    [[nodiscard]] VkBuffer Handle() const noexcept { return _handle; }
    [[nodiscard]] size_t   Size()   const noexcept { return _info.size; }
    [[nodiscard]] bool     Valid()  const noexcept { return _handle != VK_NULL_HANDLE; }
    explicit operator bool()        const noexcept { return Valid(); }

  private:
    VmaAllocator      _allocator  = nullptr;
    VkBuffer          _handle     = VK_NULL_HANDLE;
    VmaAllocation     _allocation = nullptr;
    VmaAllocationInfo _info       = {};
};

// ============================================================================
// Staging upload helper
// Records a CPU->GPU copy into cmd. Staging buffer lifetime must outlive
// queue execution — caller must not destroy before submit completes.
// ============================================================================

[[nodiscard]]
inline Buffer UploadToBuffer(VmaAllocator    allocator,
                             VkCommandBuffer cmd,
                             Buffer&         dst,
                             const void*     data,
                             size_t          size) noexcept {
    Buffer staging = Buffer::Create(allocator, size,
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    VMA_MEMORY_USAGE_CPU_ONLY);
    if (!staging.Valid()) return {};

    {
        auto mapped = staging.Map();
        memcpy(mapped.data, data, size);
    }

    ZHLN_BufferCopyDesc copy = {
        .src  = staging.Handle(),
        .dst  = dst.Handle(),
        .size = static_cast<VkDeviceSize>(size),
    };
    ZHLN_CmdCopyBuffer(cmd, &copy);

    // Return staging so caller controls its lifetime relative to submit
    return staging;
}

} // namespace ZHLN::Vk