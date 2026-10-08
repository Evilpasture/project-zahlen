// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Rendering.hpp"
#include "execution/RenderQueue.hpp"
#include <cstring>
#include <format>
#include <sys/types.h>
#include <vector>
#include <vk_mem_alloc.h>

namespace ZHLN::Vk {

enum class BufferCreationError : uint8_t {
    OutOfHostMemory        ZHLN_ANNOTATION(ZHLN::Description<"Out of host memory"> {}) = 1,
    OutOfDeviceMemory      ZHLN_ANNOTATION(ZHLN::Description<"Out of device memory"> {}),
    InvalidCaptureAddress  ZHLN_ANNOTATION(ZHLN::Description<"Invalid capture address"> {}),
    VulkanSubsystemFailure ZHLN_ANNOTATION(ZHLN::Description<"Vulkan subsystem failure"> {}),
};

enum class BufferMapError : uint8_t {
    NotAllocated ZHLN_ANNOTATION(ZHLN::Description<"Buffer owns no allocation to map"> {}) = 1,
    NotPersistentlyMapped ZHLN_ANNOTATION(ZHLN::Description<"Buffer was not created host-visible, so it has no persistent mapping"> {}),
};

enum class ImageCreationError : uint8_t {
    OutOfHostMemory        ZHLN_ANNOTATION(ZHLN::Description<"Out of host memory"> {}) = 1,
    OutOfDeviceMemory      ZHLN_ANNOTATION(ZHLN::Description<"Out of device memory"> {}),
    InvalidCaptureAddress  ZHLN_ANNOTATION(ZHLN::Description<"Invalid capture address"> {}),
    VulkanSubsystemFailure ZHLN_ANNOTATION(ZHLN::Description<"Vulkan subsystem failure"> {}),
    InvalidConfiguration   ZHLN_ANNOTATION(ZHLN::Description<"Invalid image configuration"> {}),
};

enum class AllocatorError : uint8_t {
    InitializationFailed ZHLN_ANNOTATION(ZHLN::Description<"Vulkan memory allocator initialization failed"> {}) = 1,
};

enum class StagingRingBufferError : uint8_t {
    OutOfHostMemory             ZHLN_ANNOTATION(ZHLN::Description<"Out of host memory"> {}) = 1,
    StagingBufferCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Staging ring buffer allocation failed"> {}),
};

namespace {

void SetGeneratedBufferName(VmaAllocator allocator, VkBuffer buffer, BufferUsage usage, size_t size) noexcept {
    if (!GPUAddressTracker::Get().Enabled()) {
        return;
    }

    VmaAllocatorInfo allocatorInfo {};
    vmaGetAllocatorInfo(allocator, &allocatorInfo);
    const uint64_t objectHandle = reinterpret_cast<uint64_t>(buffer);
    const std::string name = std::format("Buffer[0x{:016X},usage=0x{:08X},size={}]", objectHandle, ToVk(usage), size);
    Debug::SetObjectName(allocatorInfo.instance, allocatorInfo.device, objectHandle, VK_OBJECT_TYPE_BUFFER, name);
}

void SetGeneratedImageName(VmaAllocator allocator, VkImage image, const VkImageCreateInfo& createInfo) noexcept {
    if (!GPUAddressTracker::Get().Enabled()) {
        return;
    }

    VmaAllocatorInfo allocatorInfo {};
    vmaGetAllocatorInfo(allocator, &allocatorInfo);
    const uint64_t objectHandle = reinterpret_cast<uint64_t>(image);
    const std::string name = std::format(
        "Image[0x{:016X},fmt={},{}x{}x{},mips={},layers={}]", objectHandle, static_cast<uint32_t>(createInfo.format), createInfo.extent.width,
        createInfo.extent.height, createInfo.extent.depth, createInfo.mipLevels, createInfo.arrayLayers
    );
    Debug::SetObjectName(allocatorInfo.instance, allocatorInfo.device, objectHandle, VK_OBJECT_TYPE_IMAGE, name);
}

// Engine-owned MemoryUsage -> VMA. Exhaustive switch so adding an enumerator
// to MemoryUsage forces a compiler error here; we never hard-code VMA numeric
// values in headers.
[[nodiscard]] auto ToVmaUsage(MemoryUsage usage) noexcept -> VmaMemoryUsage {
    switch (usage) {
        case MemoryUsage::GPUOnly:
            return VMA_MEMORY_USAGE_GPU_ONLY;
        case MemoryUsage::CPUOnly:
            return VMA_MEMORY_USAGE_CPU_ONLY;
        case MemoryUsage::CPUToGPU:
            return VMA_MEMORY_USAGE_CPU_TO_GPU;
        case MemoryUsage::GPUToCPU:
            return VMA_MEMORY_USAGE_GPU_TO_CPU;
    }
    std::unreachable();
}
} // namespace

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

std::expected<void, Vk::Error> Allocator::Init(VkInstance instance, VkPhysicalDevice physical, VkDevice device) noexcept {
    const VmaVulkanFunctions vfuncs = {
        .vkGetInstanceProcAddr                   = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr                     = vkGetDeviceProcAddr,
        .vkGetPhysicalDeviceProperties           = vkGetPhysicalDeviceProperties,
        .vkGetPhysicalDeviceMemoryProperties     = vkGetPhysicalDeviceMemoryProperties,
        .vkAllocateMemory                        = vkAllocateMemory,
        .vkFreeMemory                            = vkFreeMemory,
        .vkMapMemory                             = vkMapMemory,
        .vkUnmapMemory                           = vkUnmapMemory,
        .vkFlushMappedMemoryRanges               = vkFlushMappedMemoryRanges,
        .vkInvalidateMappedMemoryRanges          = vkInvalidateMappedMemoryRanges,
        .vkBindBufferMemory                      = vkBindBufferMemory,
        .vkBindImageMemory                       = vkBindImageMemory,
        .vkGetBufferMemoryRequirements           = vkGetBufferMemoryRequirements,
        .vkGetImageMemoryRequirements            = vkGetImageMemoryRequirements,
        .vkCreateBuffer                          = vkCreateBuffer,
        .vkDestroyBuffer                         = vkDestroyBuffer,
        .vkCreateImage                           = vkCreateImage,
        .vkDestroyImage                          = vkDestroyImage,
        .vkCmdCopyBuffer                         = vkCmdCopyBuffer,
        .vkGetBufferMemoryRequirements2KHR       = vkGetBufferMemoryRequirements2,
        .vkGetImageMemoryRequirements2KHR        = vkGetImageMemoryRequirements2,
        .vkBindBufferMemory2KHR                  = vkBindBufferMemory2,
        .vkBindImageMemory2KHR                   = vkBindImageMemory2,
        .vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2,
        .vkGetDeviceBufferMemoryRequirements     = vkGetDeviceBufferMemoryRequirements,
        .vkGetDeviceImageMemoryRequirements      = vkGetDeviceImageMemoryRequirements,
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

std::expected<void, Vk::Error> Allocator::Init(const Context& ctx) noexcept {
    return Init(ctx.Instance().Handle(), ctx.Physical(), ctx.Device());
}

void Allocator::DestroyBuffer(Buffer& buffer) const noexcept {
    DestroyBuffer(_handle, buffer);
}
void Allocator::DestroyImage(Image& image) const noexcept {
    DestroyImage(_handle, image);
}

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
    _mappedData(std::exchange(other._mappedData, nullptr)), _requestedSize(std::exchange(other._requestedSize, 0)) {
}

auto Buffer::operator=(Buffer&& other) noexcept -> Buffer& {
    if (this != &other) {
        ZHLN::Assert(!Valid(), "Buffer move assignment requires the previous allocation to be explicitly retired");
        _handle        = std::exchange(other._handle, VK_NULL_HANDLE);
        _allocation    = std::exchange(other._allocation, nullptr);
        _mappedData    = std::exchange(other._mappedData, nullptr);
        _requestedSize = std::exchange(other._requestedSize, 0);
    }
    return *this;
}

auto Buffer::Release() noexcept -> std::pair<VkBuffer, VmaAllocation> {
    _mappedData    = nullptr;
    _requestedSize = 0;
    return {std::exchange(_handle, VK_NULL_HANDLE), std::exchange(_allocation, nullptr)};
}

Image::Image(Image&& other) noexcept:
    _handle(std::exchange(other._handle, VK_NULL_HANDLE)), _allocation(std::exchange(other._allocation, nullptr)),
    _config(std::exchange(other._config, ImageConfig {})) {
}

auto Image::operator=(Image&& other) noexcept -> Image& {
    if (this != &other) {
        ZHLN::Assert(!Valid(), "Image move assignment requires the previous allocation to be explicitly retired");
        _handle     = std::exchange(other._handle, VK_NULL_HANDLE);
        _allocation = std::exchange(other._allocation, nullptr);
        _config     = std::exchange(other._config, ImageConfig {});
    }
    return *this;
}

auto Image::Release() noexcept -> std::pair<VkImage, VmaAllocation> {
    _config = {};
    return {std::exchange(_handle, VK_NULL_HANDLE), std::exchange(_allocation, nullptr)};
}

auto Buffer::Create(Allocator& allocator, size_t size, BufferUsage usage, MemoryUsage memUsage) noexcept -> std::expected<Buffer, Vk::Error> {
    return Create(allocator, size, usage, memUsage, 0);
}

auto Buffer::Create(Allocator& allocator, size_t size, BufferUsage usage, MemoryUsage memUsage, VkDeviceSize minAlignment) noexcept
    -> std::expected<Buffer, Vk::Error> {
    return Create(allocator, size, usage, memUsage, minAlignment, VK_SHARING_MODE_EXCLUSIVE, {});
}

auto Buffer::Create(
    Allocator&                allocatorRef,
    size_t                    size,
    BufferUsage               usage,
    MemoryUsage               memUsage,
    VkDeviceSize              minAlignment,
    VkSharingMode             sharingMode,
    std::span<const uint32_t> queueFamilyIndices
) noexcept -> std::expected<Buffer, Vk::Error> {
    VmaAllocator      allocator = allocatorRef.Handle();
    VkBuffer          buffer    = VK_NULL_HANDLE;
    VmaAllocation     alloc     = nullptr;
    VmaAllocationInfo info      = {};

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
        .usage          = ToVmaUsage(memUsage),
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

    SetGeneratedBufferName(allocator, buffer, usage, size);

    Buffer b;
    b._handle        = buffer;
    b._allocation    = alloc;
    b._mappedData    = info.pMappedData;
    b._requestedSize = size;
    return b;
}

void Buffer::Flush(Allocator& allocatorRef, VkDeviceSize offset, VkDeviceSize size) noexcept {
    VmaAllocator allocator = allocatorRef.Handle();
    if (Valid()) {
        vmaFlushAllocation(allocator, _allocation, offset, size);
    }
}

Buffer::MappedRegion::MappedRegion(Allocator& alloc, VmaAllocation allocation, void* ptr, size_t size) noexcept:
    _ptr(ptr), _size(size), _allocator(alloc.Handle()), _allocation(allocation) {
}

Buffer::MappedRegion::~MappedRegion() noexcept {
    Cleanup();
}

// The mapping itself belongs to the allocation (it was created with
// VMA_ALLOCATION_CREATE_MAPPED_BIT) and lives until the buffer is destroyed, so only
// the write-back is ours to do here.
void Buffer::MappedRegion::Cleanup() noexcept {
    if (_allocator != nullptr && _allocation != nullptr) {
        vmaFlushAllocation(_allocator, _allocation, 0, VK_WHOLE_SIZE);
    }
    _allocator  = nullptr;
    _allocation = nullptr;
    _ptr        = nullptr;
    _size       = 0;
}

Buffer::MappedRegion::MappedRegion(MappedRegion&& other) noexcept:
    _ptr(std::exchange(other._ptr, nullptr)), _size(std::exchange(other._size, 0)), _allocator(std::exchange(other._allocator, nullptr)),
    _allocation(std::exchange(other._allocation, nullptr)) {
}

auto Buffer::MappedRegion::operator=(MappedRegion&& other) noexcept -> MappedRegion& {
    if (this != &other) {
        Cleanup();
        _ptr        = std::exchange(other._ptr, nullptr);
        _size       = std::exchange(other._size, 0);
        _allocator  = std::exchange(other._allocator, nullptr);
        _allocation = std::exchange(other._allocation, nullptr);
    }
    return *this;
}

auto Buffer::Map(Allocator& allocatorRef) noexcept -> std::expected<MappedRegion, Vk::Error> {
    if (!Valid()) {
        return std::unexpected(BufferMapError::NotAllocated);
    }
    if (_mappedData == nullptr) {
        return std::unexpected(BufferMapError::NotPersistentlyMapped);
    }
    return MappedRegion {allocatorRef, _allocation, _mappedData, _requestedSize};
}

auto UploadToBuffer(Allocator& allocatorRef, VkCommandBuffer cmd, Buffer& dst, const void* data, size_t size) noexcept -> Buffer {
    if (data == nullptr || size == 0 || !dst.Valid() || cmd == VK_NULL_HANDLE) {
        return {};
    }

    auto staging_res = Buffer::Create(allocatorRef, size, BufferUsage::TransferSrc, MemoryUsage::CPUOnly);
    if (!staging_res) {
        return {};
    }

    Buffer staging = std::move(*staging_res);

    {
        auto mapped = staging.Map(allocatorRef);
        if (!mapped) {
            allocatorRef.DestroyBuffer(staging);
            return {};
        }
        std::memcpy(mapped->Data(), data, size);
    } // `mapped` destructs here, flushing the staging writes before the copy is recorded

    CopyBuffer(cmd, staging, dst, static_cast<VkDeviceSize>(size));
    return staging;
}

auto Image::Create(Allocator& allocatorRef, const ImageConfig& config) noexcept -> std::expected<Image, Vk::Error> {
    using enum ImageCreationError;
    if (!allocatorRef.Valid() || config.format == VK_FORMAT_UNDEFINED || config.extent.width == 0 || config.extent.height == 0 ||
        config.extent.depth == 0 || config.mipLevels == 0 || config.arrayLayers == 0 || config.usage == ImageUsage::None) {
        return std::unexpected(InvalidConfiguration);
    }

    VkImageType imageType = VK_IMAGE_TYPE_2D;
    switch (config.dimension) {
        case ImageDimension::Texture2D:
            if (config.extent.depth != 1) {
                return std::unexpected(InvalidConfiguration);
            }
            imageType = VK_IMAGE_TYPE_2D;
            break;
        case ImageDimension::Texture3D:
            if (config.arrayLayers != 1 || config.cubeCompatible || config.samples != VK_SAMPLE_COUNT_1_BIT) {
                return std::unexpected(InvalidConfiguration);
            }
            imageType = VK_IMAGE_TYPE_3D;
            break;
        default:
            return std::unexpected(InvalidConfiguration);
    }

    if (config.cubeCompatible &&
        (config.dimension != ImageDimension::Texture2D || config.extent.width != config.extent.height || config.arrayLayers < 6 || config.arrayLayers % 6 != 0)) {
        return std::unexpected(InvalidConfiguration);
    }
    if (config.samples != VK_SAMPLE_COUNT_1_BIT && config.mipLevels != 1) {
        return std::unexpected(InvalidConfiguration);
    }

    const VkImageCreateInfo info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = config.cubeCompatible ? static_cast<VkImageCreateFlags>(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) : VkImageCreateFlags {},
        .imageType = imageType,
        .format = config.format,
        .extent = config.extent,
        .mipLevels = config.mipLevels,
        .arrayLayers = config.arrayLayers,
        .samples = config.samples,
        .tiling = config.tiling,
        .usage = ToVk(config.usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VmaAllocator                  allocator  = allocatorRef.Handle();
    VkImage                       img        = VK_NULL_HANDLE;
    VmaAllocation                 alloc      = nullptr;
    const VmaAllocationCreateInfo alloc_info = {
        .flags = {},
        .usage = ToVmaUsage(config.memory),
        .requiredFlags = {},
        .preferredFlags = {},
        .memoryTypeBits = {},
        .pool = {},
        .pUserData = {},
        .priority = {},
        .minAlignment = {},
    };

    const VkResult res = vmaCreateImage(allocator, &info, &alloc_info, &img, &alloc, nullptr);
    if (res != VK_SUCCESS) [[unlikely]] {
        switch (res) {
            case VK_ERROR_OUT_OF_HOST_MEMORY: return std::unexpected(OutOfHostMemory);
            case VK_ERROR_OUT_OF_DEVICE_MEMORY: return std::unexpected(OutOfDeviceMemory);
            case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return std::unexpected(InvalidCaptureAddress);
            default: return std::unexpected(VulkanSubsystemFailure);
        }
    }

    SetGeneratedImageName(allocator, img, info);
    Image result;
    result._handle = img;
    result._allocation = alloc;
    result._config = config;
    return result;
}

auto ImageView::Create(VkDevice device, const VkImageViewCreateInfo& info) -> std::expected<ImageView, Vk::Error> {
    using enum ImageViewCreationError;
    if (device == VK_NULL_HANDLE || info.sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO || info.image == VK_NULL_HANDLE ||
        info.format == VK_FORMAT_UNDEFINED || info.subresourceRange.aspectMask == VK_IMAGE_ASPECT_NONE || info.subresourceRange.levelCount == 0 ||
        info.subresourceRange.layerCount == 0) {
        return std::unexpected(InvalidConfiguration);
    }

    VkImageView view = VK_NULL_HANDLE;
    const VkResult result = vkCreateImageView(device, &info, nullptr, &view);
    if (result != VK_SUCCESS) {
        switch (result) {
            case VK_ERROR_OUT_OF_HOST_MEMORY: return std::unexpected(OutOfHostMemory);
            case VK_ERROR_OUT_OF_DEVICE_MEMORY: return std::unexpected(OutOfDeviceMemory);
            default: return std::unexpected(CreationFailed);
        }
    }
    return ImageView {device, view, info};
}

auto Image::CreateView(VkDevice device, const ImageViewConfig& config) const -> std::expected<ImageView, Vk::Error> {
    using enum ImageViewCreationError;
    const ImageConfig& image = _config;
    if (!Valid() || device == VK_NULL_HANDLE) {
        return std::unexpected(InvalidConfiguration);
    }

    const uint32_t layerCount = config.layerCount == 0 ? image.arrayLayers - std::min(config.baseLayer, image.arrayLayers) : config.layerCount;
    const uint32_t mipCount = config.mipCount == 0 ? image.mipLevels - std::min(config.baseMip, image.mipLevels) : config.mipCount;
    if (config.baseLayer >= image.arrayLayers || config.baseMip >= image.mipLevels || layerCount == 0 || mipCount == 0 ||
        layerCount > image.arrayLayers - config.baseLayer || mipCount > image.mipLevels - config.baseMip) {
        return std::unexpected(InvalidConfiguration);
    }

    ImageViewKind kind = config.kind;
    if (kind == ImageViewKind::Inferred) {
        if (image.dimension == ImageDimension::Texture3D) {
            kind = ImageViewKind::Texture3D;
        } else if (image.cubeCompatible && layerCount == 6) {
            kind = ImageViewKind::Cube;
        } else if (image.cubeCompatible && layerCount > 6) {
            kind = ImageViewKind::CubeArray;
        } else if (layerCount > 1) {
            kind = ImageViewKind::Texture2DArray;
        } else {
            kind = ImageViewKind::Texture2D;
        }
    }

    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
    switch (kind) {
        case ImageViewKind::Texture2D:
            if (image.dimension != ImageDimension::Texture2D || layerCount != 1) return std::unexpected(InvalidConfiguration);
            viewType = VK_IMAGE_VIEW_TYPE_2D;
            break;
        case ImageViewKind::Texture2DArray:
            if (image.dimension != ImageDimension::Texture2D) return std::unexpected(InvalidConfiguration);
            viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            break;
        case ImageViewKind::Texture3D:
            if (image.dimension != ImageDimension::Texture3D || config.baseLayer != 0 || layerCount != 1) return std::unexpected(InvalidConfiguration);
            viewType = VK_IMAGE_VIEW_TYPE_3D;
            break;
        case ImageViewKind::Cube:
            if (!image.cubeCompatible || config.baseLayer % 6 != 0 || layerCount != 6) return std::unexpected(InvalidConfiguration);
            viewType = VK_IMAGE_VIEW_TYPE_CUBE;
            break;
        case ImageViewKind::CubeArray:
            if (!image.cubeCompatible || config.baseLayer % 6 != 0 || layerCount < 6 || layerCount % 6 != 0) return std::unexpected(InvalidConfiguration);
            viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            break;
        case ImageViewKind::Inferred:
        default:
            return std::unexpected(InvalidConfiguration);
    }

    VkImageAspectFlags aspect = GetFormatAspect(image.format);
    switch (config.aspect) {
        case ImageAspect::Inferred: break;
        case ImageAspect::Color: aspect = VK_IMAGE_ASPECT_COLOR_BIT; break;
        case ImageAspect::Depth: aspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
        case ImageAspect::Stencil: aspect = VK_IMAGE_ASPECT_STENCIL_BIT; break;
        case ImageAspect::DepthStencil: aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT; break;
        default: return std::unexpected(InvalidConfiguration);
    }
    const VkImageAspectFlags formatAspect = GetFormatAspect(image.format);
    if (aspect == VK_IMAGE_ASPECT_NONE || (aspect & formatAspect) != aspect) {
        return std::unexpected(InvalidConfiguration);
    }

    const VkImageViewCreateInfo info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = _handle,
        .viewType = viewType,
        .format = image.format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = {
            .aspectMask = aspect,
            .baseMipLevel = config.baseMip,
            .levelCount = mipCount,
            .baseArrayLayer = config.baseLayer,
            .layerCount = layerCount,
        },
    };
    return ImageView::Create(device, info);
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

auto StagingRingBuffer::Init(Allocator& allocator, VkDevice device, VkQueue queue, uint32_t queueFamily, VkDeviceSize capacity) noexcept
    -> std::expected<void, Vk::Error> {
    Cleanup();
    _allocator   = &allocator;
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

    auto staging_res = Buffer::Create(*_allocator, _capacity, BufferUsage::TransferSrc, MemoryUsage::CPUOnly);
    if (!staging_res.has_value()) {
        _timelineSemaphore = {};
        return std::unexpected(StagingRingBufferError::StagingBufferCreationFailed);
    }
    _stagingBuffer = std::move(*staging_res);

    auto mapped = _stagingBuffer.Map(*_allocator);
    if (!mapped) {
        Cleanup();
        return std::unexpected(mapped.error());
    }
    _mappedRegion = std::move(*mapped);
    _mappedPtr    = _mappedRegion.Data();
    return {};
}

void StagingRingBuffer::Cleanup() noexcept {
    if (_device != VK_NULL_HANDLE) {
        if (_stagingBuffer.Valid()) {
            vkDeviceWaitIdle(_device);
        }
        _mappedRegion = {};
        if (_allocator != nullptr) {
            Allocator::DestroyBuffer(_allocator->Handle(), _stagingBuffer);
        }
        for (auto& rp: _retiredPools) {
            vkDestroyCommandPool(_device, rp.pool, nullptr);
        }
        _retiredPools.clear();
        _timelineSemaphore = {};
        _activeAllocations.clear();
        _mappedPtr = nullptr;
        _device    = VK_NULL_HANDLE;
        _allocator = nullptr;
        _queue     = VK_NULL_HANDLE;
        _capacity  = 0;
        _head = _tail  = 0;
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

    return {
        .slice = BufferSlice {_stagingBuffer.Handle(), 0, aligned_head, size}, .mappedData = static_cast<char*>(_mappedPtr) + aligned_head, .timelineValue = 0
    };
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

DeletionQueue::~DeletionQueue() {
    Drain();
}

void DeletionQueue::Drain() noexcept {
    Lock(_mutex, [&] {
        for (auto& queue: _queues) {
            CleanupQueue(queue);
        }
    });
}

void DeletionQueue::Enqueue(Buffer&& buffer) noexcept {
    if (!buffer.Valid())
        return;
    Lock(_mutex, [&] {
        const auto [handle, allocation] = buffer.Release();
        _queues[_currentFrameIndex].push_back({.type = DeferredDeletionEntry::Type::Buffer, .allocation = allocation, .buffer = handle});
    });
}

void DeletionQueue::Enqueue(Image&& image) noexcept {
    if (!image.Valid())
        return;
    Lock(_mutex, [&] {
        const auto [handle, allocation] = image.Release();
        _queues[_currentFrameIndex].push_back({.type = DeferredDeletionEntry::Type::Image, .allocation = allocation, .image = handle});
    });
}

void DeletionQueue::EnqueueAccelerationStructure(VkDevice device, AccelerationStructure&& handle) noexcept {
    if (!handle.Valid())
        return;
    Lock(_mutex, [&] {
        _queues[_currentFrameIndex].push_back(
            {.type = DeferredDeletionEntry::Type::AccelerationStructure, .device = device, .accelerationStructure = handle.Release()}
        );
    });
}

void DeletionQueue::EnqueuePipeline(VkDevice device, VkPipeline pipeline) noexcept {
    if (pipeline == VK_NULL_HANDLE)
        return;
    Lock(_mutex, [&] { _queues[_currentFrameIndex].push_back({.type = DeferredDeletionEntry::Type::Pipeline, .device = device, .pipeline = pipeline}); });
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
                vmaDestroyBuffer(_allocator != nullptr ? _allocator->Handle() : nullptr, entry.buffer, entry.allocation);
                break;
            case DeferredDeletionEntry::Type::Image:
                vmaDestroyImage(_allocator != nullptr ? _allocator->Handle() : nullptr, entry.image, entry.allocation);
                break;
            case DeferredDeletionEntry::Type::AccelerationStructure:
                vkDestroyAccelerationStructureKHR(entry.device, entry.accelerationStructure, nullptr);
                break;
            case DeferredDeletionEntry::Type::Pipeline:
                vkDestroyPipeline(entry.device, entry.pipeline, nullptr);
                break;
        }
    }
    queue.clear();
}

} // namespace ZHLN::Vk
