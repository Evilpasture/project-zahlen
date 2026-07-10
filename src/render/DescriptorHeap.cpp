// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/DescriptorHeap.cpp

#include "DescriptorHeap.hpp"
#include "Allocator.hpp"
#include "Rendering.hpp"
#include <print>
#include <utility>
#include <vector>

namespace ZHLN::Vk {

// ============================================================================
// DescriptorHeap Implementation
// ============================================================================

DescriptorHeap::~DescriptorHeap() noexcept {
    Cleanup();
}

DescriptorHeap::DescriptorHeap(DescriptorHeap&& other) noexcept:
    _device(std::exchange(other._device, VK_NULL_HANDLE)), _type(std::exchange(other._type, DescriptorHeapType::Resource)),
    _capacity(std::exchange(other._capacity, 0)), _stride(std::exchange(other._stride, 0)), _buffer(std::move(other._buffer)),
    _mappedRegion(std::move(other._mappedRegion)), _mappedPtr(std::exchange(other._mappedPtr, nullptr)), _deviceAddress(std::exchange(other._deviceAddress, 0)),
    _vkCmdBindSamplerHeapEXT(std::exchange(other._vkCmdBindSamplerHeapEXT, nullptr)),
    _vkCmdBindResourceHeapEXT(std::exchange(other._vkCmdBindResourceHeapEXT, nullptr)),
    _vkWriteSamplerDescriptorsEXT(std::exchange(other._vkWriteSamplerDescriptorsEXT, nullptr)),
    _vkWriteResourceDescriptorsEXT(std::exchange(other._vkWriteResourceDescriptorsEXT, nullptr)) {
}

auto DescriptorHeap::operator=(DescriptorHeap&& other) noexcept -> DescriptorHeap& {
    if (this != &other) {
        Cleanup();
        _device                        = std::exchange(other._device, VK_NULL_HANDLE);
        _type                          = std::exchange(other._type, DescriptorHeapType::Resource);
        _capacity                      = std::exchange(other._capacity, 0);
        _stride                        = std::exchange(other._stride, 0);
        _buffer                        = std::move(other._buffer);
        _mappedRegion                  = std::move(other._mappedRegion);
        _mappedPtr                     = std::exchange(other._mappedPtr, nullptr);
        _deviceAddress                 = std::exchange(other._deviceAddress, 0);
        _vkCmdBindSamplerHeapEXT       = std::exchange(other._vkCmdBindSamplerHeapEXT, nullptr);
        _vkCmdBindResourceHeapEXT      = std::exchange(other._vkCmdBindResourceHeapEXT, nullptr);
        _vkWriteSamplerDescriptorsEXT  = std::exchange(other._vkWriteSamplerDescriptorsEXT, nullptr);
        _vkWriteResourceDescriptorsEXT = std::exchange(other._vkWriteResourceDescriptorsEXT, nullptr);
    }
    return *this;
}

void DescriptorHeap::Cleanup() noexcept {
    _mappedRegion  = {};
    _buffer        = {};
    _mappedPtr     = nullptr;
    _deviceAddress = 0;
    _capacity      = 0;
    _stride        = 0;
}

auto DescriptorHeap::Init(const Context& ctx, Allocator& allocator, DescriptorHeapType type, uint32_t capacity) noexcept -> bool {
    _device   = ctx.Device();
    _type     = type;
    _capacity = capacity;

    _vkCmdBindSamplerHeapEXT       = reinterpret_cast<PFN_vkCmdBindSamplerHeapEXT>(vkGetDeviceProcAddr(_device, "vkCmdBindSamplerHeapEXT"));
    _vkCmdBindResourceHeapEXT      = reinterpret_cast<PFN_vkCmdBindResourceHeapEXT>(vkGetDeviceProcAddr(_device, "vkCmdBindResourceHeapEXT"));
    _vkWriteSamplerDescriptorsEXT  = reinterpret_cast<PFN_vkWriteSamplerDescriptorsEXT>(vkGetDeviceProcAddr(_device, "vkWriteSamplerDescriptorsEXT"));
    _vkWriteResourceDescriptorsEXT = reinterpret_cast<PFN_vkWriteResourceDescriptorsEXT>(vkGetDeviceProcAddr(_device, "vkWriteResourceDescriptorsEXT"));

    VkPhysicalDeviceDescriptorHeapPropertiesEXT props = {
        .sType                                   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT,
        .pNext                                   = nullptr,
        .samplerHeapAlignment                    = {},
        .resourceHeapAlignment                   = {},
        .maxSamplerHeapSize                      = {},
        .maxResourceHeapSize                     = {},
        .minSamplerHeapReservedRange             = {},
        .minSamplerHeapReservedRangeWithEmbedded = {},
        .minResourceHeapReservedRange            = {},
        .samplerDescriptorSize                   = {},
        .imageDescriptorSize                     = {},
        .bufferDescriptorSize                    = {},
        .samplerDescriptorAlignment              = {},
        .imageDescriptorAlignment                = {},
        .bufferDescriptorAlignment               = {},
        .maxPushDataSize                         = {},
        .imageCaptureReplayOpaqueDataSize        = {},
        .maxDescriptorHeapEmbeddedSamplers       = {},
        .samplerYcbcrConversionCount             = {},
        .sparseDescriptorHeaps                   = {},
        .protectedDescriptorHeaps                = {},
    };

    VkPhysicalDeviceProperties2 props2 = {
        .sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext      = &props,
        .properties = {},
    };
    vkGetPhysicalDeviceProperties2(ctx.Physical(), &props2);

    if (type == DescriptorHeapType::Sampler) {
        _stride = AlignUp(props.samplerDescriptorSize, props.samplerDescriptorAlignment);
    } else {
        const VkDeviceSize maxSize  = std::max(props.bufferDescriptorSize, props.imageDescriptorSize);
        const VkDeviceSize maxAlign = std::max(props.bufferDescriptorAlignment, props.imageDescriptorAlignment);
        _stride                     = AlignUp(maxSize, maxAlign);
    }

    const VkDeviceSize totalBytes = _stride * _capacity;

    _buffer = Buffer::Create(
        allocator.Get(), totalBytes, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU
    );

    if (!_buffer.Valid()) {
        return false;
    }

    _mappedRegion  = _buffer.Map();
    _mappedPtr     = _mappedRegion.data;
    _deviceAddress = GetBufferDeviceAddress(_device, _buffer.Handle());

    return _mappedPtr != nullptr && _deviceAddress != 0;
}

void DescriptorHeap::Bind(VkCommandBuffer cmd) const noexcept {
    if (!Valid()) {
        return;
    }

    const VkBindHeapInfoEXT info = {
        .sType               = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .pNext               = nullptr,
        .heapRange           = {.address = _deviceAddress, .size = _capacity * _stride},
        .reservedRangeOffset = 0,
        .reservedRangeSize   = 0
    };

    if (_type == DescriptorHeapType::Sampler) {
        _vkCmdBindSamplerHeapEXT(cmd, &info);
    } else {
        _vkCmdBindResourceHeapEXT(cmd, &info);
    }
}

// ============================================================================
// ResourceWriteBatch Implementation (PIMPL)
// ============================================================================

struct ResourceWriteBatch::Impl {
    std::vector<VkImageDescriptorInfoEXT> imageInfos;
    std::vector<VkDeviceAddressRangeEXT>  addressRanges;
    std::vector<uint32_t>                 slots;
    std::vector<VkDescriptorType>         types;
};

ResourceWriteBatch::ResourceWriteBatch() noexcept: _impl(std::make_unique<Impl>()) {
}
ResourceWriteBatch::~ResourceWriteBatch() noexcept = default;

ResourceWriteBatch::ResourceWriteBatch(ResourceWriteBatch&& other) noexcept                    = default;
auto ResourceWriteBatch::operator=(ResourceWriteBatch&& other) noexcept -> ResourceWriteBatch& = default;

void ResourceWriteBatch::AddImage(uint32_t slot, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout, VkDescriptorType type) noexcept {
    _impl->imageInfos.push_back({.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT, .pNext = nullptr, .pView = &viewInfo, .layout = layout});
    _impl->slots.push_back(slot);
    _impl->types.push_back(type);
}

void ResourceWriteBatch::AddBuffer(uint32_t slot, VkDeviceAddress address, VkDeviceSize size, VkDescriptorType type) noexcept {
    _impl->addressRanges.push_back({.address = address, .size = size});
    _impl->slots.push_back(slot);
    _impl->types.push_back(type);
}

void ResourceWriteBatch::Flush(VkDevice device, PFN_vkWriteResourceDescriptorsEXT writeFn, void* mappedPtr, VkDeviceSize stride) noexcept {
    const auto totalCount = static_cast<uint32_t>(_impl->slots.size());
    if (totalCount == 0 || (writeFn == nullptr)) {
        return;
    }

    std::vector<VkResourceDescriptorInfoEXT> resourceInfos(totalCount);
    std::vector<VkHostAddressRangeEXT>       ranges(totalCount);

    uint32_t imgIdx = 0;
    uint32_t bufIdx = 0;

    for (uint32_t i = 0; i < totalCount; ++i) {
        resourceInfos[i] = {
            .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
            .pNext = nullptr,
            .type  = _impl->types[i],
            .data  = {},
        };

        if (_impl->types[i] == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || _impl->types[i] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
            _impl->types[i] == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            resourceInfos[i].data.pImage = &_impl->imageInfos[imgIdx++];
        } else {
            resourceInfos[i].data.pAddressRange = &_impl->addressRanges[bufIdx++];
        }

        ranges[i] = {.address = static_cast<uint8_t*>(mappedPtr) + (_impl->slots[i] * stride), .size = stride};
    }

    writeFn(device, totalCount, resourceInfos.data(), ranges.data());

    _impl->imageInfos.clear();
    _impl->addressRanges.clear();
    _impl->slots.clear();
    _impl->types.clear();
}

// ============================================================================
// SamplerWriteBatch Implementation (PIMPL)
// ============================================================================

struct SamplerWriteBatch::Impl {
    std::vector<VkSamplerCreateInfo> createInfos;
    std::vector<uint32_t>            slots;
};

SamplerWriteBatch::SamplerWriteBatch() noexcept: _impl(std::make_unique<Impl>()) {
}
SamplerWriteBatch::~SamplerWriteBatch() noexcept = default;

SamplerWriteBatch::SamplerWriteBatch(SamplerWriteBatch&& other) noexcept                    = default;
auto SamplerWriteBatch::operator=(SamplerWriteBatch&& other) noexcept -> SamplerWriteBatch& = default;

void SamplerWriteBatch::AddSampler(uint32_t slot, const VkSamplerCreateInfo& createInfo) noexcept {
    _impl->createInfos.push_back(createInfo);
    _impl->slots.push_back(slot);
}

void SamplerWriteBatch::Flush(VkDevice device, PFN_vkWriteSamplerDescriptorsEXT writeFn, void* mappedPtr, VkDeviceSize stride) noexcept {
    const auto totalCount = static_cast<uint32_t>(_impl->slots.size());
    if (totalCount == 0 || (writeFn == nullptr)) {
        return;
    }

    std::vector<VkHostAddressRangeEXT> ranges(totalCount);
    for (uint32_t i = 0; i < totalCount; ++i) {
        ranges[i] = {.address = static_cast<uint8_t*>(mappedPtr) + (_impl->slots[i] * stride), .size = stride};
    }

    writeFn(device, totalCount, _impl->createInfos.data(), ranges.data());

    _impl->createInfos.clear();
    _impl->slots.clear();
}

// ============================================================================
// SlotAllocator Implementation (PIMPL)
// ============================================================================

struct SlotAllocator::Impl {
    uint32_t              capacity = 0;
    uint32_t              nextSlot = 0;
    std::vector<uint32_t> freeSlots;
};

SlotAllocator::SlotAllocator() noexcept: _impl(std::make_unique<Impl>()) {
}
SlotAllocator::~SlotAllocator() noexcept = default;

SlotAllocator::SlotAllocator(SlotAllocator&& other) noexcept                    = default;
auto SlotAllocator::operator=(SlotAllocator&& other) noexcept -> SlotAllocator& = default;

void SlotAllocator::Init(uint32_t capacity) noexcept {
    _impl->capacity = capacity;
    _impl->nextSlot = 0;
    _impl->freeSlots.clear();
}

auto SlotAllocator::Allocate() noexcept -> uint32_t {
    if (!_impl->freeSlots.empty()) {
        const uint32_t slot = _impl->freeSlots.back();
        _impl->freeSlots.pop_back();
        return slot;
    }
    if (_impl->nextSlot < _impl->capacity) {
        return _impl->nextSlot++;
    }
    std::println(stderr, "[SlotAllocator] Heap section is completely full (Capacity: {})", _impl->capacity);
    std::abort();
}

void SlotAllocator::Free(uint32_t slot) noexcept {
    if (slot < _impl->nextSlot) {
        _impl->freeSlots.push_back(slot);
    }
}

void SlotAllocator::Clear() noexcept {
    _impl->nextSlot = 0;
    _impl->freeSlots.clear();
}

// ============================================================================
// HeapManager Implementation
// ============================================================================

auto HeapManager::Init(
    const Context& ctx,
    Allocator&     allocator,
    uint32_t       staticResourceCount,
    uint32_t       dynamicResourceCount,
    uint32_t       staticSamplerCount,
    uint32_t       dynamicSamplerCount,
    uint32_t       doubleBufferCount
) noexcept -> bool {
    _staticResourceCount  = staticResourceCount;
    _dynamicResourceCount = dynamicResourceCount;
    _staticSamplerCount   = staticSamplerCount;
    _dynamicSamplerCount  = dynamicSamplerCount;
    _doubleBufferCount    = doubleBufferCount;
    _currentFrameIndex    = 0;

    _staticResourceAlloc.Init(staticResourceCount);
    _staticSamplerAlloc.Init(staticSamplerCount);

    const uint32_t totalResourceCount = staticResourceCount + (doubleBufferCount * dynamicResourceCount);
    const uint32_t totalSamplerCount  = staticSamplerCount + (doubleBufferCount * dynamicSamplerCount);

    bool ok = _resourceHeap.Init(ctx, allocator, DescriptorHeapType::Resource, totalResourceCount);
    if (ok) {
        ok = _samplerHeap.Init(ctx, allocator, DescriptorHeapType::Sampler, totalSamplerCount);
    }

    return ok;
}

void HeapManager::BeginFrame(uint32_t frameIndex) noexcept {
    _currentFrameIndex        = frameIndex % _doubleBufferCount;
    _dynamicResourceAllocated = 0;
    _dynamicSamplerAllocated  = 0;
}

auto HeapManager::AllocateStaticResourceSlot() noexcept -> uint32_t {
    return _staticResourceAlloc.Allocate();
}

void HeapManager::FreeStaticResourceSlot(uint32_t slot) noexcept {
    _staticResourceAlloc.Free(slot);
}

auto HeapManager::AllocateStaticSamplerSlot() noexcept -> uint32_t {
    return _staticSamplerAlloc.Allocate();
}

void HeapManager::FreeStaticSamplerSlot(uint32_t slot) noexcept {
    _staticSamplerAlloc.Free(slot);
}

auto HeapManager::AllocateDynamicResourceRange(uint32_t count) noexcept -> uint32_t {
    const uint32_t baseSlot = _staticResourceCount + (_currentFrameIndex * _dynamicResourceCount) + _dynamicResourceAllocated;
    if (_dynamicResourceAllocated + count > _dynamicResourceCount) [[unlikely]] {
        std::println(
            stderr, "[HeapManager] Resource Dynamic Heap overflow: allocated {} but max is {}", _dynamicResourceAllocated + count, _dynamicResourceCount
        );
        std::abort();
    }
    _dynamicResourceAllocated += count;
    return baseSlot;
}

auto HeapManager::AllocateDynamicSamplerRange(uint32_t count) noexcept -> uint32_t {
    const uint32_t baseSlot = _staticSamplerCount + (_currentFrameIndex * _dynamicSamplerCount) + _dynamicSamplerAllocated;
    if (_dynamicSamplerAllocated + count > _dynamicSamplerCount) [[unlikely]] {
        std::println(stderr, "[HeapManager] Sampler Dynamic Heap overflow: allocated {} but max is {}", _dynamicSamplerAllocated + count, _dynamicSamplerCount);
        std::abort();
    }
    _dynamicSamplerAllocated += count;
    return baseSlot;
}

void HeapManager::FlushResourceBatch(ResourceWriteBatch& batch) noexcept {
    batch.Flush(_resourceHeap.GetDevice(), _resourceHeap.GetWriteResourceDescriptorsFn(), _resourceHeap.GetMappedPtr(), _resourceHeap.GetStride());
}

void HeapManager::FlushSamplerBatch(SamplerWriteBatch& batch) noexcept {
    batch.Flush(_samplerHeap.GetDevice(), _samplerHeap.GetWriteSamplerDescriptorsFn(), _samplerHeap.GetMappedPtr(), _samplerHeap.GetStride());
}

void HeapManager::BindHeaps(VkCommandBuffer cmd) const noexcept {
    _resourceHeap.Bind(cmd);
    _samplerHeap.Bind(cmd);
}

} // namespace ZHLN::Vk
