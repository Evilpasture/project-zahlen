// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Allocator.cpp

#include "RenderQueue.hpp"
#include "Rendering.hpp"
#include <cstring>
#include <sys/types.h>
#include <vector>

// ============================================================================
// Private Allocator Errors (Tier 1)
// Declared at file scope in this translation unit: no header exposes them, so
// external code only ever logs the type-erased ZHLN::Error message. File scope
// (rather than an anonymous namespace) keeps their reflected category names
// stable for both native reflection and the AST transpiler fallback.
// ============================================================================

namespace ZHLN::Vk {

// Private allocator/creation errors (Tier 1): declared at file scope in this
// translation unit so no header exposes them; callers only log the
// type-erased ZHLN::Error message.
enum class BufferCreationError : uint8_t {
    OutOfHostMemory ZHLN_ANNOTATION(ZHLN::Description<"Out of host memory">{}) = 1,
    OutOfDeviceMemory ZHLN_ANNOTATION(ZHLN::Description<"Out of device memory">{}),
    InvalidCaptureAddress ZHLN_ANNOTATION(ZHLN::Description<"Invalid capture address">{}),
    VulkanSubsystemFailure ZHLN_ANNOTATION(ZHLN::Description<"Vulkan subsystem failure">{}),
};

enum class ImageCreationError : uint8_t {
    OutOfHostMemory ZHLN_ANNOTATION(ZHLN::Description<"Out of host memory">{}) = 1,
    OutOfDeviceMemory ZHLN_ANNOTATION(ZHLN::Description<"Out of device memory">{}),
    InvalidCaptureAddress ZHLN_ANNOTATION(ZHLN::Description<"Invalid capture address">{}),
    VulkanSubsystemFailure ZHLN_ANNOTATION(ZHLN::Description<"Vulkan subsystem failure">{}),
};

// VMA allocator instance bring-up failure.
enum class AllocatorError : uint8_t {
    InitializationFailed ZHLN_ANNOTATION(ZHLN::Description<"Vulkan memory allocator initialization failed">{}) = 1,
};

// Staging ring buffer / persistent transfer buffer bring-up failures.
enum class StagingRingBufferError : uint8_t {
    OutOfHostMemory ZHLN_ANNOTATION(ZHLN::Description<"Out of host memory">{}) = 1,
    StagingBufferCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Staging ring buffer allocation failed">{}),
};


// ============================================================================
// Allocator RAII
// ============================================================================

Allocator::~Allocator() noexcept {
    if (_handle != nullptr) {
        vmaDestroyAllocator(_handle);
    }
}

Allocator::Allocator(Allocator&& other) noexcept: _handle(std::exchange(other._handle, nullptr)) {
}

auto Allocator::operator=(Allocator&& other) noexcept -> Allocator& {
    if (this != &other) {
        if (_handle != nullptr) {
            vmaDestroyAllocator(_handle);
        }
        _handle = std::exchange(other._handle, nullptr);
    }
    return *this;
}

std::expected<void, ZHLN::Error> Allocator::Init(VkInstance instance, VkPhysicalDevice physical, VkDevice device) noexcept {
    const VmaVulkanFunctions vfuncs = {
        .vkGetInstanceProcAddr                    = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr                      = vkGetDeviceProcAddr,
        .vkGetPhysicalDeviceProperties            = vkGetPhysicalDeviceProperties,
        .vkGetPhysicalDeviceMemoryProperties      = vkGetPhysicalDeviceMemoryProperties,
        .vkAllocateMemory                         = vkAllocateMemory,
        .vkFreeMemory                             = vkFreeMemory,
        .vkMapMemory                              = vkMapMemory,
        .vkUnmapMemory                            = vkUnmapMemory,
        .vkFlushMappedMemoryRanges                = vkFlushMappedMemoryRanges,
        .vkInvalidateMappedMemoryRanges           = vkInvalidateMappedMemoryRanges,
        .vkBindBufferMemory                       = vkBindBufferMemory,
        .vkBindImageMemory                        = vkBindImageMemory,
        .vkGetBufferMemoryRequirements            = vkGetBufferMemoryRequirements,
        .vkGetImageMemoryRequirements             = vkGetImageMemoryRequirements,
        .vkCreateBuffer                           = vkCreateBuffer,
        .vkDestroyBuffer                          = vkDestroyBuffer,
        .vkCreateImage                            = vkCreateImage,
        .vkDestroyImage                           = vkDestroyImage,
        .vkCmdCopyBuffer                          = vkCmdCopyBuffer,
        .vkGetBufferMemoryRequirements2KHR        = vkGetBufferMemoryRequirements2,
        .vkGetImageMemoryRequirements2KHR         = vkGetImageMemoryRequirements2,
        .vkBindBufferMemory2KHR                   = vkBindBufferMemory2,
        .vkBindImageMemory2KHR                    = vkBindImageMemory2,
        .vkGetPhysicalDeviceMemoryProperties2KHR  = vkGetPhysicalDeviceMemoryProperties2,
        .vkGetDeviceBufferMemoryRequirements      = vkGetDeviceBufferMemoryRequirements,
        .vkGetDeviceImageMemoryRequirements       = vkGetDeviceImageMemoryRequirements,
        .vkGetMemoryWin32HandleKHR               = nullptr,
#if VMA_GET_PHYSICAL_DEVICE_PROPERTIES2
        .vkGetPhysicalDeviceProperties2KHR = nullptr,
#endif
    };

    const VmaAllocatorCreateInfo info = {
        .flags                          = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice                 = physical,
        .device                         = device,
        .preferredLargeHeapBlockSize    = 0,
        .pAllocationCallbacks           = nullptr,
        .pDeviceMemoryCallbacks         = nullptr,
        .pHeapSizeLimit                 = nullptr,
        .pVulkanFunctions               = &vfuncs,
        .instance                       = instance,
        .vulkanApiVersion               = VK_API_VERSION_1_3,
        .pTypeExternalMemoryHandleTypes = {},
    };

    VkResult res = vmaCreateAllocator(&info, &_handle);
    if (res != VK_SUCCESS) {
        return std::unexpected(AllocatorError::InitializationFailed);
    }
    return {};
}

std::expected<void, ZHLN::Error> Allocator::Init(const Context& ctx) noexcept {
    return Init(ctx.Instance(), ctx.Physical(), ctx.Device());
}

// ============================================================================
// Buffer RAII
// ============================================================================
auto Buffer::Create(VmaAllocator allocator, size_t size, VkBufferUsageFlags usage, VmaMemoryUsage memUsage) noexcept -> std::expected<Buffer, Error> {
    return Create(allocator, size, usage, memUsage, 0);
}

auto Buffer::Create(VmaAllocator allocator, size_t size, VkBufferUsageFlags usage, VmaMemoryUsage memUsage, VkDeviceSize minAlignment) noexcept
    -> std::expected<Buffer, Error> {
    VkBuffer          buffer = VK_NULL_HANDLE;
    VmaAllocation     alloc  = nullptr;
    VmaAllocationInfo info   = {};

    // Acceleration-structure addresses must be aligned to 256 bytes. Promote
    // the caller's optional alignment here so every AS buffer gets the same
    // guarantee, even when a call site uses the four-argument overload.
    VkDeviceSize effectiveAlignment = minAlignment;
    if (usage & (VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR)) {
        effectiveAlignment = std::max(effectiveAlignment, static_cast<VkDeviceSize>(256));
    }

    const VkBufferCreateInfo buffer_info = {
        .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext                 = nullptr,
        .flags                 = 0,
        .size                  = size,
        .usage                 = usage,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices   = nullptr
    };

    VmaAllocationCreateInfo alloc_info = {
        .flags          = 0,
        .usage          = memUsage,
        .requiredFlags  = 0,
        .preferredFlags = 0,
        .memoryTypeBits = 0,
        .pool           = nullptr,
        .pUserData      = nullptr,
        .priority       = 0.0F,
        .minAlignment   = effectiveAlignment
    };

    // Automatically request persistent mapping for host-visible memory types
    if (memUsage == VMA_MEMORY_USAGE_CPU_ONLY || memUsage == VMA_MEMORY_USAGE_CPU_TO_GPU || memUsage == VMA_MEMORY_USAGE_GPU_TO_CPU) {
        alloc_info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    // VMA 3.1+: honor additional alignment requirements of the allocation
    // (VmaAllocationCreateInfo::minAlignment only exists from VMA 3.1.0).
#if defined(VMA_VERSION_MAJOR) && defined(VMA_VERSION_MINOR) && (VMA_VERSION_MAJOR > 3 || (VMA_VERSION_MAJOR == 3 && VMA_VERSION_MINOR >= 1))
    alloc_info.minAlignment = effectiveAlignment;
#endif

    VkResult res = vmaCreateBuffer(allocator, &buffer_info, &alloc_info, &buffer, &alloc, &info);
    if (res != VK_SUCCESS) [[unlikely]] {
        switch (res) {
            case VK_ERROR_OUT_OF_HOST_MEMORY:
                return std::unexpected(BufferCreationError::OutOfHostMemory);

            case VK_ERROR_OUT_OF_DEVICE_MEMORY:
                return std::unexpected(BufferCreationError::OutOfDeviceMemory);

            case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS:
                return std::unexpected(BufferCreationError::InvalidCaptureAddress);

            default:
                // Native driver/subsystem failure: preserve raw code in a dedicated failure state
                return std::unexpected(BufferCreationError::VulkanSubsystemFailure);
        }
    }

    Buffer b;
    b._handle        = {allocator, buffer, alloc};
    b._info          = info;
    b._requestedSize = size;
    return b;
}

void Buffer::Flush(VkDeviceSize offset, VkDeviceSize size) noexcept {
    if (_handle.Allocator() != nullptr && _handle.Allocation() != nullptr) {
        vmaFlushAllocation(_handle.Allocator(), _handle.Allocation(), offset, size);
    }
}

Buffer::MappedRegion::MappedRegion(VmaAllocator alloc, VmaAllocation allocation, void* ptr) noexcept: data(ptr), _handle(alloc, ptr, allocation) {
}

Buffer::MappedRegion::MappedRegion(MappedRegion&& other) noexcept: data(std::exchange(other.data, nullptr)), _handle(std::move(other._handle)) {
}

auto Buffer::MappedRegion::operator=(MappedRegion&& other) noexcept -> MappedRegion& {
    if (this != &other) {
        data    = std::exchange(other.data, nullptr);
        _handle = std::move(other._handle);
    }
    return *this;
}

auto Buffer::Map() noexcept -> MappedRegion {
    // If the buffer is already persistently mapped, bypass expensive vmaMapMemory system calls
    if (_info.pMappedData != nullptr) {
        return {nullptr, nullptr, _info.pMappedData};
    }
    void* ptr = nullptr;
    vmaMapMemory(_handle.Allocator(), _handle.Allocation(), &ptr);
    return {_handle.Allocator(), _handle.Allocation(), ptr};
}

// ============================================================================
// UploadToBuffer Implementation
// ============================================================================

auto UploadToBuffer(VmaAllocator allocator, VkCommandBuffer cmd, Buffer& dst, const void* data, size_t size) noexcept -> Buffer {
    auto staging_res = Buffer::Create(allocator, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    if (!staging_res.has_value()) {
        return {};
    }
    Buffer staging = std::move(staging_res.value());

    {
        auto mapped = staging.Map();
        if (mapped.data != nullptr) {
            std::memcpy(mapped.data, data, size);
        }
    }

    // Cleaned up to utilize the updated CopyBuffer helper
    CopyBuffer(cmd, staging, dst, static_cast<VkDeviceSize>(size));
    return staging;
}

// ============================================================================
// Image RAII
// ============================================================================

auto Image::Create(VmaAllocator allocator, const VkImageCreateInfo& info, VmaMemoryUsage memUsage) -> std::expected<Image, Error> {
    VkImage                       img        = VK_NULL_HANDLE;
    VmaAllocation                 alloc      = nullptr;
    const VmaAllocationCreateInfo alloc_info = {
        .flags          = {},
        .usage          = memUsage,
        .requiredFlags  = {},
        .preferredFlags = {},
        .memoryTypeBits = {},
        .pool           = {},
        .pUserData      = {},
        .priority       = {},
        .minAlignment   = {}
    };

    VkResult res = vmaCreateImage(allocator, &info, &alloc_info, &img, &alloc, nullptr);
    if (res != VK_SUCCESS) [[unlikely]] {
        switch (res) {
            case VK_ERROR_OUT_OF_HOST_MEMORY:
                return std::unexpected(ImageCreationError::OutOfHostMemory);

            case VK_ERROR_OUT_OF_DEVICE_MEMORY:
                return std::unexpected(ImageCreationError::OutOfDeviceMemory);

            case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS:
                return std::unexpected(ImageCreationError::InvalidCaptureAddress);

            default:
                return std::unexpected(ImageCreationError::VulkanSubsystemFailure);
        }
    }

    Image r;
    r._handle = {allocator, img, alloc};
    return r;
}

ImageBuilder::ImageBuilder() noexcept {
    _info = {
        .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext                 = nullptr,
        .flags                 = 0,
        .imageType             = VK_IMAGE_TYPE_2D,
        .format                = VK_FORMAT_UNDEFINED,
        .extent                = {.width = 0, .height = 0, .depth = 0},
        .mipLevels             = 1,
        .arrayLayers           = 1,
        .samples               = VK_SAMPLE_COUNT_1_BIT,
        .tiling                = VK_IMAGE_TILING_OPTIMAL,
        .usage                 = 0,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices   = nullptr,
        .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED
    };
}

auto ImageBuilder::Type(VkImageType type) noexcept -> ImageBuilder& {
    _info.imageType = type;
    return *this;
}

auto ImageBuilder::Format(VkFormat format) noexcept -> ImageBuilder& {
    _info.format = format;
    return *this;
}

auto ImageBuilder::Dimensions(uint32_t width, uint32_t height, uint32_t depth) noexcept -> ImageBuilder& {
    _info.extent = {.width = width, .height = height, .depth = depth};
    return *this;
}

auto ImageBuilder::Mips(uint32_t levels) noexcept -> ImageBuilder& {
    _info.mipLevels = levels;
    return *this;
}

auto ImageBuilder::Layers(uint32_t layers) noexcept -> ImageBuilder& {
    _info.arrayLayers = layers;
    return *this;
}

auto ImageBuilder::Samples(VkSampleCountFlagBits samples) noexcept -> ImageBuilder& {
    _info.samples = samples;
    return *this;
}

auto ImageBuilder::Tiling(VkImageTiling tiling) noexcept -> ImageBuilder& {
    _info.tiling = tiling;
    return *this;
}

auto ImageBuilder::Usage(VkImageUsageFlags usage) noexcept -> ImageBuilder& {
    _info.usage = usage;
    return *this;
}

auto ImageBuilder::SharingMode(VkSharingMode mode) noexcept -> ImageBuilder& {
    _info.sharingMode = mode;
    return *this;
}

auto ImageBuilder::Flags(VkImageCreateFlags flags) noexcept -> ImageBuilder& {
    _info.flags = flags;
    return *this;
}

auto ImageBuilder::Texture2D(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, uint32_t mips) noexcept -> ImageBuilder& {
    _info.imageType   = VK_IMAGE_TYPE_2D;
    _info.format      = format;
    _info.extent      = {.width = width, .height = height, .depth = 1};
    _info.mipLevels   = mips;
    _info.arrayLayers = 1;
    _info.usage       = usage;
    return *this;
}

auto ImageBuilder::TextureCube(uint32_t size, VkFormat format, VkImageUsageFlags usage, uint32_t mips) noexcept -> ImageBuilder& {
    _info.flags       = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    _info.imageType   = VK_IMAGE_TYPE_2D;
    _info.format      = format;
    _info.extent      = {.width = size, .height = size, .depth = 1};
    _info.mipLevels   = mips;
    _info.arrayLayers = 6;
    _info.usage       = usage;
    return *this;
}

auto ImageBuilder::Build(VmaAllocator allocator, VmaMemoryUsage memUsage) const noexcept -> std::expected<Image, Error> {
    return Image::Create(allocator, _info, memUsage);
}

// ============================================================================
// StagingRingBuffer Implementation
// ============================================================================

StagingRingBuffer::StagingRingBuffer(StagingRingBuffer&& other) noexcept:
    _allocator(std::exchange(other._allocator, nullptr)), _device(std::exchange(other._device, VK_NULL_HANDLE)),
    _queue(std::exchange(other._queue, VK_NULL_HANDLE)), _queueFamily(std::exchange(other._queueFamily, 0xFFFFFFFF)),
    _stagingBuffer(std::move(other._stagingBuffer)), _mappedRegion(std::move(other._mappedRegion)), _mappedPtr(std::exchange(other._mappedPtr, nullptr)),
    _capacity(std::exchange(other._capacity, 0)), _head(std::exchange(other._head, 0)), _tail(std::exchange(other._tail, 0)),
    _timelineSemaphore(std::move(other._timelineSemaphore)), _timelineValue(std::exchange(other._timelineValue, 0)),
    _activeAllocations(std::move(other._activeAllocations)), _retiredPools(std::move(other._retiredPools)) {
}

auto StagingRingBuffer::operator=(StagingRingBuffer&& other) noexcept -> StagingRingBuffer& {
    if (this != &other) {
        Cleanup();
        _allocator         = std::exchange(other._allocator, nullptr);
        _device            = std::exchange(other._device, VK_NULL_HANDLE);
        _queue             = std::exchange(other._queue, VK_NULL_HANDLE);
        _queueFamily       = std::exchange(other._queueFamily, 0xFFFFFFFF);
        _stagingBuffer     = std::move(other._stagingBuffer);
        _mappedRegion      = std::move(other._mappedRegion);
        _mappedPtr         = std::exchange(other._mappedPtr, nullptr);
        _capacity          = std::exchange(other._capacity, 0);
        _head              = std::exchange(other._head, 0);
        _tail              = std::exchange(other._tail, 0);
        _timelineSemaphore = std::move(other._timelineSemaphore);
        _timelineValue     = std::exchange(other._timelineValue, 0);
        _activeAllocations = std::move(other._activeAllocations);
        _retiredPools      = std::move(other._retiredPools);
    }
    return *this;
}

auto StagingRingBuffer::Init(VmaAllocator allocator, VkDevice device, VkQueue queue, uint32_t queueFamily, VkDeviceSize capacity) noexcept
    -> std::expected<void, ZHLN::Error> {
    _allocator   = allocator;
    _device      = device;
    _queue       = queue;
    _queueFamily = queueFamily;
    _capacity    = capacity;

    VkSemaphoreTypeCreateInfo type_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .pNext = nullptr, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0
    };
    VkSemaphoreCreateInfo sem_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type_info, .flags = 0};

    VkSemaphore raw_sem = VK_NULL_HANDLE;
    auto        res     = vkCreateSemaphore(_device, &sem_info, nullptr, &raw_sem);
    if (res != VK_SUCCESS) {
        return std::unexpected(StagingRingBufferError::OutOfHostMemory);
    }
    _timelineSemaphore = Semaphore(_device, raw_sem); // Adopt into RAII wrapper

    auto staging_res = Buffer::Create(_allocator, _capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    if (!staging_res.has_value()) {
        _timelineSemaphore = {}; // Triggers automatic destruction logic
        return std::unexpected(StagingRingBufferError::StagingBufferCreationFailed);
    }
    _stagingBuffer = std::move(*staging_res);

    _mappedRegion = _stagingBuffer.Map();
    _mappedPtr    = _mappedRegion.data;
    return {};
}

void StagingRingBuffer::Cleanup() noexcept {
    if (_device != VK_NULL_HANDLE) {
        _mappedRegion  = {}; // Clean up mapping wrapper
        _stagingBuffer = {}; // Clean up buffer handle
        for (auto& rp: _retiredPools) {
            vkDestroyCommandPool(_device, rp.pool, nullptr);
        }
        _retiredPools.clear();
        _timelineSemaphore = {}; // Trigger automatic RAII cleanup
    }
}

void StagingRingBuffer::Recycle() noexcept {
    if (!_timelineSemaphore.Valid()) {
        return;
    }

    uint64_t completed_value = 0;
    vkGetSemaphoreCounterValue(_device, _timelineSemaphore.Get(), &completed_value);

    // Guard against unsubmitted allocations (timelineValue == 0)
    while (!_activeAllocations.empty() && _activeAllocations.front().timelineValue > 0 && _activeAllocations.front().timelineValue <= completed_value) {
        _tail = (_activeAllocations.front().offset + _activeAllocations.front().size) % _capacity;
        _activeAllocations.erase(_activeAllocations.begin());
    }

    for (auto it = _retiredPools.begin(); it != _retiredPools.end();) {
        if (it->timelineValue <= completed_value) {
            vkDestroyCommandPool(_device, it->pool, nullptr);
            it = _retiredPools.erase(it);
        } else {
            ++it;
        }
    }
}

void StagingRingBuffer::RetirePool(VkCommandPool pool, uint64_t timelineValue) noexcept {
    _retiredPools.push_back({.pool = pool, .timelineValue = timelineValue});
}

auto StagingRingBuffer::Allocate(VkDeviceSize size, VkDeviceSize alignment) noexcept -> Allocation {
    Recycle();

    VkDeviceSize aligned_head = (_head + alignment - 1) & ~(alignment - 1);
    bool         wrap         = false;

    if (aligned_head + size > _capacity) {
        aligned_head = 0;
        wrap         = true;
    }

    while (true) {
        bool has_space = false;

        if (_activeAllocations.empty()) {
            has_space = true;
        } else if (_tail <= _head) {
            if (wrap) {
                has_space = (size < _tail);
            } else {
                has_space = true;
            }
        } else {
            if (wrap) {
                has_space = false; // Cannot wrap if active region already wraps
            } else {
                has_space = (aligned_head + size < _tail);
            }
        }

        if (has_space) {
            break;
        }

        if (_activeAllocations.empty()) {
            return {}; // Out of memory boundaries
        }

        uint64_t wait_val = _activeAllocations.front().timelineValue;
        if (wait_val == 0) {
            break; // Avoid waiting on unsubmitted allocations
        }

        VkSemaphore         sem_handle = _timelineSemaphore.Get();
        VkSemaphoreWaitInfo wait_info  = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .pNext = nullptr, .flags = 0, .semaphoreCount = 1, .pSemaphores = &sem_handle, .pValues = &wait_val
        };
        vkWaitSemaphores(_device, &wait_info, UINT64_MAX);
        Recycle();
    }

    _head = aligned_head + size;
    _activeAllocations.push_back({.offset = aligned_head, .size = size, .timelineValue = 0});

    return {.buffer = _stagingBuffer.Handle(), .offset = aligned_head, .mappedData = static_cast<char*>(_mappedPtr) + aligned_head, .timelineValue = 0};
}

auto StagingRingBuffer::Submit(VkCommandBuffer cmd, VkFence fence) noexcept -> uint64_t {
    _timelineValue++;

    for (auto& alloc: _activeAllocations) {
        if (alloc.timelineValue == 0) {
            alloc.timelineValue = _timelineValue;
        }
    }

    if (auto res = QueueSubmit(
            _queue, cmd, VK_NULL_HANDLE, 0, VK_PIPELINE_STAGE_2_NONE, _timelineSemaphore.Get(), _timelineValue, VK_PIPELINE_STAGE_2_COPY_BIT, fence
        );
        !res) [[unlikely]] {
        return 0;
    }

    return _timelineValue;
}

thread_local DeletionQueue* t_active_deletion_queue = nullptr;

void DeferVmaDestruction(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation) noexcept {
    if (t_active_deletion_queue != nullptr) {
        t_active_deletion_queue->EnqueueBuffer(allocator, buffer, allocation);
    }
}

void DeferVmaDestruction(VmaAllocator allocator, VkImage image, VmaAllocation allocation) noexcept {
    if (t_active_deletion_queue != nullptr) {
        t_active_deletion_queue->EnqueueImage(allocator, image, allocation);
    }
}

// ============================================================================
// DeletionQueue Implementation
// ============================================================================

DeletionQueue::~DeletionQueue() {
    for (auto& queue: _queues) {
        CleanupQueue(queue);
    }
}

void DeletionQueue::Init(uint32_t doubleBufferCount) noexcept {
    _queues.resize(doubleBufferCount);
    _currentFrameIndex = 0;
}

void DeletionQueue::EnqueueBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation) noexcept {
    _queues[_currentFrameIndex].push_back({.type = DeferredDeletionEntry::Type::Buffer, .allocator = allocator, .allocation = allocation, .buffer = buffer});
}

void DeletionQueue::EnqueueImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation) noexcept {
    _queues[_currentFrameIndex].push_back({.type = DeferredDeletionEntry::Type::Image, .allocator = allocator, .allocation = allocation, .image = image});
}

void DeletionQueue::BeginFrame(uint32_t frameIndex) noexcept {
    _currentFrameIndex = frameIndex % _queues.size();
    // Safe to reclaim memory now! The fence for this frame slot has finished [c].
    CleanupQueue(_queues[_currentFrameIndex]);
}

void DeletionQueue::CleanupQueue(std::vector<DeferredDeletionEntry>& queue) noexcept {
    for (const auto& entry: queue) {
        if (entry.type == DeferredDeletionEntry::Type::Buffer) {
            vmaDestroyBuffer(entry.allocator, entry.buffer, entry.allocation);
        } else {
            vmaDestroyImage(entry.allocator, entry.image, entry.allocation);
        }
    }
    queue.clear();
}

} // namespace ZHLN::Vk
