// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "execution/RenderQueue.hpp"
#include "Rendering.hpp"
#include <cstring>
#include <sys/types.h>
#include <vector>


namespace ZHLN::Vk {

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

enum class AllocatorError : uint8_t {
    InitializationFailed ZHLN_ANNOTATION(ZHLN::Description<"Vulkan memory allocator initialization failed">{}) = 1,
};

enum class StagingRingBufferError : uint8_t {
    OutOfHostMemory ZHLN_ANNOTATION(ZHLN::Description<"Out of host memory">{}) = 1,
    StagingBufferCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Staging ring buffer allocation failed">{}),
};



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

std::expected<void, ZHLN::ErrorCode> Allocator::Init(VkInstance instance, VkPhysicalDevice physical, VkDevice device) noexcept {
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

std::expected<void, ZHLN::ErrorCode> Allocator::Init(const Context& ctx) noexcept {
    return Init(ctx.Instance(), ctx.Physical(), ctx.Device());
}

void Allocator::DestroyBuffer(Buffer& buffer) const noexcept { DestroyBuffer(_handle, buffer); }
void Allocator::DestroyImage(Image& image) const noexcept { DestroyImage(_handle, image); }

void Allocator::DestroyBuffer(VmaAllocator allocator, Buffer& buffer) noexcept {
    if (buffer.Valid()) {
        const auto [handle, allocation] = buffer.Release();
        vmaDestroyBuffer(allocator, handle, allocation);
    }
}

void Allocator::DestroyImage(VmaAllocator allocator, Image& image) noexcept {
    if (image.Valid()) {
        const auto [handle, allocation] = image.Release();
        vmaDestroyImage(allocator, handle, allocation);
    }
}

Buffer::Buffer(Buffer&& other) noexcept:
    _handle(std::exchange(other._handle, VK_NULL_HANDLE)), _allocation(std::exchange(other._allocation, nullptr)),
    _info(std::exchange(other._info, VmaAllocationInfo {})), _requestedSize(std::exchange(other._requestedSize, 0)) {}

auto Buffer::operator=(Buffer&& other) noexcept -> Buffer& {
    if (this != &other) {
        ZHLN::Assert(!Valid(), "Buffer move assignment requires the previous allocation to be explicitly retired");
        _handle        = std::exchange(other._handle, VK_NULL_HANDLE);
        _allocation    = std::exchange(other._allocation, nullptr);
        _info          = std::exchange(other._info, VmaAllocationInfo {});
        _requestedSize = std::exchange(other._requestedSize, 0);
    }
    return *this;
}

auto Buffer::Release() noexcept -> std::pair<VkBuffer, VmaAllocation> {
    _info = {};
    _requestedSize = 0;
    return {std::exchange(_handle, VK_NULL_HANDLE), std::exchange(_allocation, nullptr)};
}

Image::Image(Image&& other) noexcept:
    _handle(std::exchange(other._handle, VK_NULL_HANDLE)), _allocation(std::exchange(other._allocation, nullptr)) {}

auto Image::operator=(Image&& other) noexcept -> Image& {
    if (this != &other) {
        ZHLN::Assert(!Valid(), "Image move assignment requires the previous allocation to be explicitly retired");
        _handle     = std::exchange(other._handle, VK_NULL_HANDLE);
        _allocation = std::exchange(other._allocation, nullptr);
    }
    return *this;
}

auto Image::Release() noexcept -> std::pair<VkImage, VmaAllocation> {
    return {std::exchange(_handle, VK_NULL_HANDLE), std::exchange(_allocation, nullptr)};
}

auto Buffer::Create(VmaAllocator allocator, size_t size, BufferUsage usage, MemoryUsage memUsage) noexcept -> std::expected<Buffer, ErrorCode> {
    return Create(allocator, size, usage, memUsage, 0);
}

auto Buffer::Create(VmaAllocator allocator, size_t size, BufferUsage usage, MemoryUsage memUsage, VkDeviceSize minAlignment) noexcept
    -> std::expected<Buffer, ErrorCode> {
    return Create(allocator, size, usage, memUsage, minAlignment, VK_SHARING_MODE_EXCLUSIVE, {});
}

auto Buffer::Create(
    VmaAllocator              allocator,
    size_t                    size,
    BufferUsage               usage,
    MemoryUsage               memUsage,
    VkDeviceSize              minAlignment,
    VkSharingMode             sharingMode,
    std::span<const uint32_t> queueFamilyIndices
) noexcept -> std::expected<Buffer, ErrorCode> {
    VkBuffer          buffer = VK_NULL_HANDLE;
    VmaAllocation     alloc  = nullptr;
    VmaAllocationInfo info   = {};

    VkDeviceSize effectiveAlignment = minAlignment;
    if (Has(usage, BufferUsage::AccelerationStructureStorage | BufferUsage::AccelerationStructureBuildInput)) {
        effectiveAlignment = std::max(effectiveAlignment, static_cast<VkDeviceSize>(256));
    }

    const VkBufferCreateInfo buffer_info = {
        .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext                 = nullptr,
        .flags                 = 0,
        .size                  = size,
        .usage                 = ToVk(usage),
        .sharingMode           = sharingMode,
        .queueFamilyIndexCount = static_cast<uint32_t>(queueFamilyIndices.size()),
        .pQueueFamilyIndices   = queueFamilyIndices.empty() ? nullptr : queueFamilyIndices.data()
    };

    VmaAllocationCreateInfo alloc_info = {
        .flags          = 0,
        .usage          = ToVma(memUsage),
        .requiredFlags  = 0,
        .preferredFlags = 0,
        .memoryTypeBits = 0,
        .pool           = nullptr,
        .pUserData      = nullptr,
        .priority       = 0.0F,
        .minAlignment   = effectiveAlignment
    };

    if (memUsage == MemoryUsage::CPUOnly || memUsage == MemoryUsage::CPUToGPU || memUsage == MemoryUsage::GPUToCPU) {
        alloc_info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

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
                return std::unexpected(BufferCreationError::VulkanSubsystemFailure);
        }
    }

    Buffer b;
    b._handle        = buffer;
    b._allocation    = alloc;
    b._info          = info;
    b._requestedSize = size;
    return b;
}

void Buffer::Flush(VmaAllocator allocator, VkDeviceSize offset, VkDeviceSize size) noexcept {
    if (Valid()) {
        vmaFlushAllocation(allocator, _allocation, offset, size);
    }
}

Buffer::MappedRegion::MappedRegion(VmaAllocator alloc, VmaAllocation allocation, void* ptr) noexcept:
    data(ptr), _allocator(alloc), _allocation(allocation) {}

Buffer::MappedRegion::~MappedRegion() noexcept { Cleanup(); }

void Buffer::MappedRegion::Cleanup() noexcept {
    if (_allocator != nullptr && _allocation != nullptr) {
        vmaFlushAllocation(_allocator, _allocation, 0, VK_WHOLE_SIZE);
        vmaUnmapMemory(_allocator, _allocation);
    }
    _allocator = nullptr;
    _allocation = nullptr;
    data = nullptr;
}

Buffer::MappedRegion::MappedRegion(MappedRegion&& other) noexcept:
    data(std::exchange(other.data, nullptr)), _allocator(std::exchange(other._allocator, nullptr)),
    _allocation(std::exchange(other._allocation, nullptr)) {}

auto Buffer::MappedRegion::operator=(MappedRegion&& other) noexcept -> MappedRegion& {
    if (this != &other) {
        Cleanup();
        data        = std::exchange(other.data, nullptr);
        _allocator  = std::exchange(other._allocator, nullptr);
        _allocation = std::exchange(other._allocation, nullptr);
    }
    return *this;
}

auto Buffer::Map(VmaAllocator allocator) noexcept -> MappedRegion {
    if (!Valid()) {
        return {};
    }
    if (_info.pMappedData != nullptr) {
        return {nullptr, nullptr, _info.pMappedData};
    }
    void* ptr = nullptr;
    if (vmaMapMemory(allocator, _allocation, &ptr) != VK_SUCCESS) {
        return {};
    }
    return {allocator, _allocation, ptr};
}

auto UploadToBuffer(VmaAllocator allocator, VkCommandBuffer cmd, Buffer& dst, const void* data, size_t size) noexcept -> Buffer {
    auto staging_res = Buffer::Create(allocator, size, BufferUsage::TransferSrc, MemoryUsage::CPUOnly);
    if (!staging_res.has_value()) {
        return {};
    }
    Buffer staging = std::move(staging_res.value());

    bool mappedOK = false;
    {
        auto mapped = staging.Map(allocator);
        if (mapped.data != nullptr) {
            std::memcpy(mapped.data, data, size);
            mappedOK = true;
        }
    }
    if (!mappedOK) {
        Allocator::DestroyBuffer(allocator, staging);
        return {};
    }

    CopyBuffer(cmd, staging, dst, static_cast<VkDeviceSize>(size));
    return staging;
}


auto Image::Create(VmaAllocator allocator, const VkImageCreateInfo& info, MemoryUsage memUsage) -> std::expected<Image, ErrorCode> {
    VkImage                       img        = VK_NULL_HANDLE;
    VmaAllocation                 alloc      = nullptr;
    const VmaAllocationCreateInfo alloc_info = {
        .flags          = {},
        .usage          = ToVma(memUsage),
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
    r._handle = img;
    r._allocation = alloc;
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

auto ImageBuilder::Usage(ImageUsage usage) noexcept -> ImageBuilder& {
    _info.usage = ToVk(usage);
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

auto ImageBuilder::Texture2D(uint32_t width, uint32_t height, VkFormat format, ImageUsage usage, uint32_t mips) noexcept -> ImageBuilder& {
    _info.imageType   = VK_IMAGE_TYPE_2D;
    _info.format      = format;
    _info.extent      = {.width = width, .height = height, .depth = 1};
    _info.mipLevels   = mips;
    _info.arrayLayers = 1;
    _info.usage       = ToVk(usage);
    return *this;
}

auto ImageBuilder::TextureCube(uint32_t size, VkFormat format, ImageUsage usage, uint32_t mips) noexcept -> ImageBuilder& {
    _info.flags       = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    _info.imageType   = VK_IMAGE_TYPE_2D;
    _info.format      = format;
    _info.extent      = {.width = size, .height = size, .depth = 1};
    _info.mipLevels   = mips;
    _info.arrayLayers = 6;
    _info.usage       = ToVk(usage);
    return *this;
}

auto ImageBuilder::Build(VmaAllocator allocator, MemoryUsage memUsage) const noexcept -> std::expected<Image, ErrorCode> {
    return Image::Create(allocator, _info, memUsage);
}


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
    -> std::expected<void, ZHLN::ErrorCode> {
    Cleanup();
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
    _timelineSemaphore = Semaphore(_device, raw_sem);

    auto staging_res = Buffer::Create(_allocator, _capacity, BufferUsage::TransferSrc, MemoryUsage::CPUOnly);
    if (!staging_res.has_value()) {
        _timelineSemaphore = {};
        return std::unexpected(StagingRingBufferError::StagingBufferCreationFailed);
    }
    _stagingBuffer = std::move(*staging_res);

    _mappedRegion = _stagingBuffer.Map(_allocator);
    _mappedPtr    = _mappedRegion.data;
    if (_mappedPtr == nullptr) {
        Cleanup();
        return std::unexpected(StagingRingBufferError::StagingBufferCreationFailed);
    }
    return {};
}

void StagingRingBuffer::Cleanup() noexcept {
    if (_device != VK_NULL_HANDLE) {
        if (_stagingBuffer.Valid()) {
            vkDeviceWaitIdle(_device);
        }
        _mappedRegion = {};
        Allocator::DestroyBuffer(_allocator, _stagingBuffer);
        for (auto& rp: _retiredPools) {
            vkDestroyCommandPool(_device, rp.pool, nullptr);
        }
        _retiredPools.clear();
        _timelineSemaphore = {};
        _activeAllocations.clear();
        _mappedPtr = nullptr;
        _device = VK_NULL_HANDLE;
        _allocator = nullptr;
        _queue = VK_NULL_HANDLE;
        _capacity = 0;
        _head = _tail = 0;
        _timelineValue = 0;
    }
}

void StagingRingBuffer::Recycle() noexcept {
    if (!_timelineSemaphore.Valid()) {
        return;
    }

    uint64_t completed_value = 0;
    vkGetSemaphoreCounterValue(_device, _timelineSemaphore.Get(), &completed_value);

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
                has_space = false;
            } else {
                has_space = (aligned_head + size < _tail);
            }
        }

        if (has_space) {
            break;
        }

        if (_activeAllocations.empty()) {
            return {};
        }

        uint64_t wait_val = _activeAllocations.front().timelineValue;
        if (wait_val == 0) {
            break;
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

    return {.slice = BufferSlice {_stagingBuffer.Handle(), 0, aligned_head, size},
            .mappedData = static_cast<char*>(_mappedPtr) + aligned_head, .timelineValue = 0};
}

auto StagingRingBuffer::Submit(ExecutableCommands cmds, VkFence fence) noexcept -> uint64_t {
    const uint64_t nextValue = _timelineValue + 1;
    if (auto res = QueueSubmit(
            _queue, std::move(cmds), VK_NULL_HANDLE, 0, VK_PIPELINE_STAGE_2_NONE, _timelineSemaphore.Get(), nextValue, VK_PIPELINE_STAGE_2_COPY_BIT, fence
        );
        !res) [[unlikely]] {
        return 0;
    }

    _timelineValue = nextValue;
    for (auto& alloc: _activeAllocations) {
        if (alloc.timelineValue == 0) {
            alloc.timelineValue = _timelineValue;
        }
    }
    return _timelineValue;
}

DeletionQueue::~DeletionQueue() { Drain(); }

void DeletionQueue::Drain() noexcept {
    Lock(_mutex, [&] {
        for (auto& queue: _queues) {
            CleanupQueue(queue);
        }
    });
}

void DeletionQueue::Enqueue(Buffer&& buffer) noexcept {
    if (!buffer.Valid()) return;
    Lock(_mutex, [&] {
        const auto [handle, allocation] = buffer.Release();
        _queues[_currentFrameIndex].push_back({.type = DeferredDeletionEntry::Type::Buffer, .allocation = allocation, .buffer = handle});
    });
}

void DeletionQueue::Enqueue(Image&& image) noexcept {
    if (!image.Valid()) return;
    Lock(_mutex, [&] {
        const auto [handle, allocation] = image.Release();
        _queues[_currentFrameIndex].push_back({.type = DeferredDeletionEntry::Type::Image, .allocation = allocation, .image = handle});
    });
}

void DeletionQueue::EnqueueAccelerationStructure(VkDevice device, AccelerationStructure&& handle) noexcept {
    if (!handle.Valid()) return;
    Lock(_mutex, [&] {
        _queues[_currentFrameIndex].push_back({.type = DeferredDeletionEntry::Type::AccelerationStructure, .device = device,
                                               .accelerationStructure = handle.Release()});
    });
}

void DeletionQueue::EnqueuePipeline(VkDevice device, VkPipeline pipeline) noexcept {
    if (pipeline == VK_NULL_HANDLE) return;
    Lock(_mutex, [&] {
        _queues[_currentFrameIndex].push_back({.type = DeferredDeletionEntry::Type::Pipeline, .device = device, .pipeline = pipeline});
    });
}

void DeletionQueue::BeginFrame(uint32_t frameIndex) noexcept {
    Lock(_mutex, [&] {
        _currentFrameIndex = FrameSlot(frameIndex);
        CleanupQueue(_queues[_currentFrameIndex]);
    });
}

void DeletionQueue::CleanupQueue(std::vector<DeferredDeletionEntry>& queue) noexcept {
    for (const auto& entry: queue) {
        switch (entry.type) {
            case DeferredDeletionEntry::Type::Buffer:
                vmaDestroyBuffer(_allocator, entry.buffer, entry.allocation);
                break;
            case DeferredDeletionEntry::Type::Image:
                vmaDestroyImage(_allocator, entry.image, entry.allocation);
                break;
            case DeferredDeletionEntry::Type::AccelerationStructure:
                ZHLN_DestroyAS(entry.device, entry.accelerationStructure);
                break;
            case DeferredDeletionEntry::Type::Pipeline:
                ZHLN_DestroyPipeline(entry.device, entry.pipeline);
                break;
        }
    }
    queue.clear();
}

}
