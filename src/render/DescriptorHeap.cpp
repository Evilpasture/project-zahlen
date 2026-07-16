// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/DescriptorHeap.cpp

#include "DescriptorHeap.hpp"
#include "Allocator.hpp"
#include "Rendering.hpp"
#include <utility>
#include <vector>

namespace ZHLN::Vk {

// ============================================================================
// DescriptorHeap Implementation
// ============================================================================

template <DescriptorHeapType Type>
DescriptorHeap<Type>::~DescriptorHeap() noexcept {
    Cleanup();
}

template <DescriptorHeapType Type>
DescriptorHeap<Type>::DescriptorHeap(DescriptorHeap&& other) noexcept:
    _device(std::exchange(other._device, VK_NULL_HANDLE)), _capacity(std::exchange(other._capacity, 0)), _stride(std::exchange(other._stride, 0)),
    _buffer(std::move(other._buffer)), _mappedRegion(std::move(other._mappedRegion)), _mappedPtr(std::exchange(other._mappedPtr, nullptr)),
    _deviceAddress(std::exchange(other._deviceAddress, 0)), _vkCmdBindHeapEXT(std::exchange(other._vkCmdBindHeapEXT, nullptr)),
    _vkWriteDescriptorsEXT(std::exchange(other._vkWriteDescriptorsEXT, nullptr)) {
}

template <DescriptorHeapType Type>
auto DescriptorHeap<Type>::operator=(DescriptorHeap&& other) noexcept -> DescriptorHeap& {
    if (this != &other) {
        Cleanup();
        _device                = std::exchange(other._device, VK_NULL_HANDLE);
        _capacity              = std::exchange(other._capacity, 0);
        _stride                = std::exchange(other._stride, 0);
        _buffer                = std::move(other._buffer);
        _mappedRegion          = std::move(other._mappedRegion);
        _mappedPtr             = std::exchange(other._mappedPtr, nullptr);
        _deviceAddress         = std::exchange(other._deviceAddress, 0);
        _vkCmdBindHeapEXT      = std::exchange(other._vkCmdBindHeapEXT, nullptr);
        _vkWriteDescriptorsEXT = std::exchange(other._vkWriteDescriptorsEXT, nullptr);
    }
    return *this;
}

template <DescriptorHeapType Type>
void DescriptorHeap<Type>::Cleanup() noexcept {
    _mappedRegion  = {};
    _buffer        = {};
    _mappedPtr     = nullptr;
    _deviceAddress = 0;
    _capacity      = 0;
    _stride        = 0;
}

template <DescriptorHeapType Type>
auto DescriptorHeap<Type>::Init(const Context& ctx, Allocator& allocator, uint32_t capacity) noexcept -> std::expected<void, DescriptorHeapError> {
    _device   = ctx.Device();
    _capacity = capacity;

    // Load only the specific, required entry points for this specialization
    if constexpr (Type == DescriptorHeapType::Sampler) {
        _vkCmdBindHeapEXT      = reinterpret_cast<PFN_vkCmdBindSamplerHeapEXT>(vkGetDeviceProcAddr(_device, "vkCmdBindSamplerHeapEXT"));
        _vkWriteDescriptorsEXT = reinterpret_cast<PFN_vkWriteSamplerDescriptorsEXT>(vkGetDeviceProcAddr(_device, "vkWriteSamplerDescriptorsEXT"));
    } else {
        _vkCmdBindHeapEXT      = reinterpret_cast<PFN_vkCmdBindResourceHeapEXT>(vkGetDeviceProcAddr(_device, "vkCmdBindResourceHeapEXT"));
        _vkWriteDescriptorsEXT = reinterpret_cast<PFN_vkWriteResourceDescriptorsEXT>(vkGetDeviceProcAddr(_device, "vkWriteResourceDescriptorsEXT"));
    }

    if (_vkCmdBindHeapEXT == nullptr || _vkWriteDescriptorsEXT == nullptr) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::FunctionLoaderFailed);
    }

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

    // Optimized branch evaluation at compile time via constexpr
    if constexpr (Type == DescriptorHeapType::Sampler) {
        _stride = AlignUp(props.samplerDescriptorSize, props.samplerDescriptorAlignment);
    } else {
        const VkDeviceSize max_size  = std::max(props.bufferDescriptorSize, props.imageDescriptorSize);
        const VkDeviceSize max_align = std::max(props.bufferDescriptorAlignment, props.imageDescriptorAlignment);
        _stride                      = AlignUp(max_size, max_align);
    }

    const VkDeviceSize total_bytes = _stride * _capacity;

    auto buffer_res = Buffer::Create(
        allocator.Get(), total_bytes, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU
    );

    if (!buffer_res.has_value()) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::AllocationFailed);
    }
    _buffer = std::move(*buffer_res);

    _mappedRegion = _buffer.Map();
    _mappedPtr    = _mappedRegion.data;
    if (_mappedPtr == nullptr) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::MappingFailed);
    }

    _deviceAddress = GetBufferAddress(_device, _buffer.Handle());
    if (_deviceAddress == 0) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::DeviceAddressFailed);
    }

    return {};
}

template <DescriptorHeapType Type>
void DescriptorHeap<Type>::Bind(VkCommandBuffer cmd) const noexcept {
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

    _vkCmdBindHeapEXT(cmd, &info);
}

template <DescriptorHeapType Type>
void DescriptorHeap<Type>::Flush(ResourceWriteBatch& batch) noexcept
    requires(Type == DescriptorHeapType::Resource)
{
    if (Valid() && _vkWriteDescriptorsEXT != nullptr) {
        batch.Flush(_device, _vkWriteDescriptorsEXT, _mappedPtr, _stride);
    }
}

template <DescriptorHeapType Type>
void DescriptorHeap<Type>::Flush(SamplerWriteBatch& batch) noexcept
    requires(Type == DescriptorHeapType::Sampler)
{
    if (Valid() && _vkWriteDescriptorsEXT != nullptr) {
        batch.Flush(_device, _vkWriteDescriptorsEXT, _mappedPtr, _stride);
    }
}

// ============================================================================
// ResourceWriteBatch Implementation (PIMPL)
// ============================================================================

struct ResourceWriteBatch::Impl {
    std::vector<VkImageDescriptorInfoEXT> imageInfos;
    std::vector<VkImageViewCreateInfo>    viewInfos; // Local copy to protect structure lifetime
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
    // 1. Store the structure by value to keep it alive until Flush() completes
    _impl->viewInfos.push_back(viewInfo);

    // 2. Queue with pView set to nullptr for now. We will resolve stable memory pointers inside Flush()!
    _impl->imageInfos.push_back({.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT, .pNext = nullptr, .pView = nullptr, .layout = layout});
    _impl->slots.push_back(slot);
    _impl->types.push_back(type);
}

void ResourceWriteBatch::AddBuffer(uint32_t slot, VkDeviceAddress address, VkDeviceSize size, VkDescriptorType type) noexcept {
    _impl->addressRanges.push_back({.address = address, .size = size});
    _impl->slots.push_back(slot);
    _impl->types.push_back(type);
}

void ResourceWriteBatch::Flush(VkDevice device, PFN_vkWriteResourceDescriptorsEXT writeFn, void* mappedPtr, VkDeviceSize stride) noexcept {
    const auto total_count = static_cast<uint32_t>(_impl->slots.size());
    if (total_count == 0 || (writeFn == nullptr)) {
        return;
    }

    std::vector<VkResourceDescriptorInfoEXT> resource_infos(total_count);
    std::vector<VkHostAddressRangeEXT>       ranges(total_count);

    uint32_t img_idx = 0;
    uint32_t buf_idx = 0;

    for (uint32_t i = 0; i < total_count; ++i) {
        resource_infos[i] = {
            .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
            .pNext = nullptr,
            .type  = _impl->types[i],
            .data  = {},
        };

        if (_impl->types[i] == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || _impl->types[i] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
            _impl->types[i] == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            // 3. The vector has stopped growing and its memory has stabilized.
            // It is now perfectly safe to resolve and assign the direct pointer address.
            _impl->imageInfos[img_idx].pView = &_impl->viewInfos[img_idx];
            resource_infos[i].data.pImage    = &_impl->imageInfos[img_idx++];
        } else {
            resource_infos[i].data.pAddressRange = &_impl->addressRanges[buf_idx++];
        }

        ranges[i] = {.address = static_cast<uint8_t*>(mappedPtr) + (_impl->slots[i] * stride), .size = stride};
    }

    writeFn(device, total_count, resource_infos.data(), ranges.data());

    _impl->imageInfos.clear();
    _impl->viewInfos.clear(); // Safely clear out lifetime-tied structures
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

// Impl has non-trivial elements, so we must support explicit moving
SamplerWriteBatch::SamplerWriteBatch(SamplerWriteBatch&& other) noexcept                    = default;
auto SamplerWriteBatch::operator=(SamplerWriteBatch&& other) noexcept -> SamplerWriteBatch& = default;

void SamplerWriteBatch::AddSampler(uint32_t slot, const VkSamplerCreateInfo& createInfo) noexcept {
    _impl->createInfos.push_back(createInfo);
    _impl->slots.push_back(slot);
}

void SamplerWriteBatch::Flush(VkDevice device, PFN_vkWriteSamplerDescriptorsEXT writeFn, void* mappedPtr, VkDeviceSize stride) noexcept {
    const auto total_count = static_cast<uint32_t>(_impl->slots.size());
    if (total_count == 0 || (writeFn == nullptr)) {
        return;
    }

    std::vector<VkHostAddressRangeEXT> ranges(total_count);
    for (uint32_t i = 0; i < total_count; ++i) {
        ranges[i] = {.address = static_cast<uint8_t*>(mappedPtr) + (_impl->slots[i] * stride), .size = stride};
    }

    writeFn(device, total_count, _impl->createInfos.data(), ranges.data());

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
    DescriptorHeapError   errorOnExhaustion = DescriptorHeapError::ResourceSlotsExhausted;
};

SlotAllocator::SlotAllocator() noexcept: _impl(std::make_unique<Impl>()) {
}
SlotAllocator::~SlotAllocator() noexcept = default;

SlotAllocator::SlotAllocator(SlotAllocator&& other) noexcept                    = default;
auto SlotAllocator::operator=(SlotAllocator&& other) noexcept -> SlotAllocator& = default;

void SlotAllocator::Init(uint32_t capacity, DescriptorHeapError errorOnExhaustion) noexcept {
    _impl->capacity = capacity;
    _impl->nextSlot = 0;
    _impl->freeSlots.clear();
    _impl->errorOnExhaustion = errorOnExhaustion;
}

auto SlotAllocator::Allocate() noexcept -> std::expected<uint32_t, DescriptorHeapError> {
    if (!_impl->freeSlots.empty()) {
        const uint32_t slot = _impl->freeSlots.back();
        _impl->freeSlots.pop_back();
        return slot;
    }
    if (_impl->nextSlot < _impl->capacity) {
        return _impl->nextSlot++;
    }
    return std::unexpected(_impl->errorOnExhaustion);
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
) noexcept -> std::expected<void, DescriptorHeapError> {
    _staticResourceCount  = staticResourceCount;
    _dynamicResourceCount = dynamicResourceCount;
    _staticSamplerCount   = staticSamplerCount;
    _dynamicSamplerCount  = dynamicSamplerCount;
    _doubleBufferCount    = doubleBufferCount;
    _currentFrameIndex    = 0;

    _staticResourceAlloc.Init(staticResourceCount, DescriptorHeapError::ResourceSlotsExhausted);
    _staticSamplerAlloc.Init(staticSamplerCount, DescriptorHeapError::SamplerSlotsExhausted);

    const uint32_t total_resource_count = staticResourceCount + (doubleBufferCount * dynamicResourceCount);
    const uint32_t total_sampler_count  = staticSamplerCount + (doubleBufferCount * dynamicSamplerCount);

    auto res_heap_init = _resourceHeap.Init(ctx, allocator, total_resource_count);
    if (!res_heap_init.has_value()) [[unlikely]] {
        return std::unexpected(res_heap_init.error());
    }

    auto samp_heap_init = _samplerHeap.Init(ctx, allocator, total_sampler_count);
    if (!samp_heap_init.has_value()) [[unlikely]] {
        return std::unexpected(samp_heap_init.error());
    }

    return {};
}

void HeapManager::BeginFrame(uint32_t frameIndex) noexcept {
    _currentFrameIndex        = frameIndex % _doubleBufferCount;
    _dynamicResourceAllocated = 0;
    _dynamicSamplerAllocated  = 0;
}

auto HeapManager::AllocateStaticResourceSlot() noexcept -> std::expected<uint32_t, DescriptorHeapError> {
    return _staticResourceAlloc.Allocate();
}

void HeapManager::FreeStaticResourceSlot(uint32_t slot) noexcept {
    _staticResourceAlloc.Free(slot);
}

auto HeapManager::AllocateStaticSamplerSlot() noexcept -> std::expected<uint32_t, DescriptorHeapError> {
    return _staticSamplerAlloc.Allocate();
}

void HeapManager::FreeStaticSamplerSlot(uint32_t slot) noexcept {
    _staticSamplerAlloc.Free(slot);
}

auto HeapManager::AllocateDynamicResourceRange(uint32_t count) noexcept -> std::expected<uint32_t, DescriptorHeapError> {
    const uint32_t base_slot = _staticResourceCount + (_currentFrameIndex * _dynamicResourceCount) + _dynamicResourceAllocated;
    if (_dynamicResourceAllocated + count > _dynamicResourceCount) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::DynamicResourceOverflow);
    }
    _dynamicResourceAllocated += count;
    return base_slot;
}

auto HeapManager::AllocateDynamicSamplerRange(uint32_t count) noexcept -> std::expected<uint32_t, DescriptorHeapError> {
    const uint32_t base_slot = _staticSamplerCount + (_currentFrameIndex * _dynamicSamplerCount) + _dynamicSamplerAllocated;
    if (_dynamicSamplerAllocated + count > _dynamicSamplerCount) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::DynamicSamplerOverflow);
    }
    _dynamicSamplerAllocated += count;
    return base_slot;
}

void HeapManager::FlushResourceBatch(ResourceWriteBatch& batch) noexcept {
    _resourceHeap.Flush(batch);
}

void HeapManager::FlushSamplerBatch(SamplerWriteBatch& batch) noexcept {
    _samplerHeap.Flush(batch);
}

void HeapManager::BindHeaps(VkCommandBuffer cmd) const noexcept {
    _resourceHeap.Bind(cmd);
    _samplerHeap.Bind(cmd);
}

// Explicit template instantiations for class-level compilation protection
template class DescriptorHeap<DescriptorHeapType::Resource>;
template class DescriptorHeap<DescriptorHeapType::Sampler>;

} // namespace ZHLN::Vk
