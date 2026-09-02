// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/DescriptorHeap.cpp

#include "DescriptorHeap.hpp"
#include "Allocator.hpp"
#include "Rendering.hpp"
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <utility>
#include <vector>

namespace ZHLN::Vk {

namespace {

/// Refuses a batch that would write outside the heap.
///
/// batch.Flush() hands the driver `mappedPtr + slot * stride` for every slot it
/// carries, so one out-of-range slot writes over the implementation's reserved
/// range at the tail of the buffer -- and past the mapping entirely if the slot
/// is far enough out. Nothing upstream bounds it in general: regions addressed
/// by raw offset rather than through SlotAllocator (the bindless
/// globalTextures[] array, every HeapPassBindings span) compute their slot
/// arithmetically, so an unchecked index arrives here as a plausible-looking
/// number.
///
/// Dropping the batch loses a descriptor, which shows up as a wrong or missing
/// texture. That is strictly better than the alternative, and the log names the
/// slot that overflowed.
[[nodiscard]] auto BatchFitsHeap(const uint32_t* slots, uint32_t count, uint32_t maxSlot, uint32_t capacity, const char* heapName) noexcept -> bool {
    if (count == 0 || slots == nullptr || maxSlot < capacity) {
        return true;
    }
    ZHLN::Log("[DescriptorHeap] {} heap write out of range: slot {} >= capacity {}. Dropping {} descriptor write(s).", heapName, maxSlot, capacity, count);
    return false;
}

} // namespace

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
    _reservedSize(std::exchange(other._reservedSize, 0)), _buffer(std::move(other._buffer)), _mappedRegion(std::move(other._mappedRegion)),
    _mappedPtr(std::exchange(other._mappedPtr, nullptr)), _bindInfo(std::exchange(other._bindInfo, VkBindHeapInfoEXT {})),
    _vkCmdBindHeapEXT(std::exchange(other._vkCmdBindHeapEXT, nullptr)), _vkWriteDescriptorsEXT(std::exchange(other._vkWriteDescriptorsEXT, nullptr)) {
}

template <DescriptorHeapType Type>
auto DescriptorHeap<Type>::operator=(DescriptorHeap&& other) noexcept -> DescriptorHeap& {
    if (this != &other) {
        Cleanup();
        _device                = std::exchange(other._device, VK_NULL_HANDLE);
        _capacity              = std::exchange(other._capacity, 0);
        _stride                = std::exchange(other._stride, 0);
        _reservedSize          = std::exchange(other._reservedSize, 0);
        _buffer                = std::move(other._buffer);
        _mappedRegion          = std::move(other._mappedRegion);
        _mappedPtr             = std::exchange(other._mappedPtr, nullptr);
        _bindInfo              = std::exchange(other._bindInfo, VkBindHeapInfoEXT {});
        _vkCmdBindHeapEXT      = std::exchange(other._vkCmdBindHeapEXT, nullptr);
        _vkWriteDescriptorsEXT = std::exchange(other._vkWriteDescriptorsEXT, nullptr);
    }
    return *this;
}

template <DescriptorHeapType Type>
void DescriptorHeap<Type>::Cleanup() noexcept {
    _mappedRegion = {};
    _buffer       = {};
    _mappedPtr    = nullptr;
    _capacity     = 0;
    _stride       = 0;
    _reservedSize = 0;
    _bindInfo     = {};
}

template <DescriptorHeapType Type>
auto DescriptorHeap<Type>::Init(const Context& ctx, Allocator& allocator, uint32_t capacity) noexcept -> std::expected<void, Error> {
    _device   = ctx.Device();
    _capacity = capacity;

    if (!ctx.DescriptorHeapsSupported()) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::ExtensionUnavailable);
    }

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
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT,
        .pNext = nullptr,
    };

    VkPhysicalDeviceProperties2 props2 = {
        .sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext      = &props,
        .properties = {},
    };
    vkGetPhysicalDeviceProperties2(ctx.Physical(), &props2);
    _nonCoherentAtomSize = props2.properties.limits.nonCoherentAtomSize;
    if (_nonCoherentAtomSize == 0) {
        _nonCoherentAtomSize = 1;
    }

    VkDeviceSize heap_alignment = 0;
    VkDeviceSize max_heap_size  = 0;
    if constexpr (Type == DescriptorHeapType::Sampler) {
        _stride        = AlignUp(props.samplerDescriptorSize, props.samplerDescriptorAlignment);
        _reservedSize  = props.minSamplerHeapReservedRange;
        heap_alignment = props.samplerHeapAlignment;
        max_heap_size  = props.maxSamplerHeapSize;
    } else {
        // Unified stride: every resource slot must be able to hold any resource
        // descriptor and stay aligned for both image and buffer descriptors
        // (the reservedRangeOffset VUIDs demand multiples of BOTH alignments).
        const VkDeviceSize max_size  = std::max(props.bufferDescriptorSize, props.imageDescriptorSize);
        const VkDeviceSize max_align = std::max(props.bufferDescriptorAlignment, props.imageDescriptorAlignment);
        _stride                      = AlignUp(max_size, max_align);
        _reservedSize                = props.minResourceHeapReservedRange;
        heap_alignment               = props.resourceHeapAlignment;
        max_heap_size                = props.maxResourceHeapSize;
    }

    if (_stride == 0) [[unlikely]] {
        // A well-formed implementation always advertises non-zero descriptor
        // sizes; treat zero as "extension unusable".
        return std::unexpected(DescriptorHeapError::ExtensionUnavailable);
    }

    const VkDeviceSize used_bytes  = _stride * _capacity;
    const VkDeviceSize total_bytes = AlignUp(used_bytes + _reservedSize, std::max<VkDeviceSize>(heap_alignment, 1));

    if (total_bytes > max_heap_size) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::HeapTooLarge);
    }

    auto buffer_res = Buffer::Create(
        allocator.Get(), total_bytes, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU,
        std::max<VkDeviceSize>(heap_alignment, 1)
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

    const VkDeviceAddress address = GetBufferAddress(_device, _buffer.Handle());
    if (address == 0) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::DeviceAddressFailed);
    }

    // The heap binding VUIDs demand heapRange.address be a multiple of the
    // heap alignment. VMA honors minAlignment on VMA >= 3.1; verify at runtime
    // so older VMA builds fail loudly instead of binding a misaligned heap.
    if ((address % std::max<VkDeviceSize>(heap_alignment, 1)) != 0) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::DeviceAddressFailed);
    }

    // Cache the bind descriptor; the heap layout never changes after init.
    // Secondary command buffers reuse it for heap-state inheritance.
    _bindInfo = {
        .sType               = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .pNext               = nullptr,
        .heapRange           = {.address = address, .size = total_bytes},
        .reservedRangeOffset = used_bytes,
        .reservedRangeSize   = _reservedSize,
    };

    return {};
}

template <DescriptorHeapType Type>
void DescriptorHeap<Type>::FlushHostCache(VkDeviceSize offset, VkDeviceSize size) noexcept {
    // Descriptors are written by the driver directly into our persistent
    // mapping; make them coherent before the GPU binds this heap.
    _buffer.Flush(offset, size);
}

template <DescriptorHeapType Type>
void DescriptorHeap<Type>::Bind(VkCommandBuffer cmd) const noexcept {
    if (!Valid()) {
        return;
    }
    _vkCmdBindHeapEXT(cmd, &_bindInfo);
}

template <DescriptorHeapType Type>
void DescriptorHeap<Type>::Flush(ResourceWriteBatch& batch) noexcept
    requires(Type == DescriptorHeapType::Resource)
{
    if (Valid() && _vkWriteDescriptorsEXT != nullptr) {
        const auto  count = batch.SlotCount();
        const auto* slots = batch.SlotsData();
        // Resolve the dirty byte range BEFORE Flush() clears the batch's slot
        // vector (clear() does not free but we don't want to rely on that).
        VkDeviceSize flushOffset = 0;
        VkDeviceSize flushSize   = 0;
        if (count > 0 && slots != nullptr && _stride > 0) {
            const auto [minIt, maxIt] = std::minmax_element(slots, slots + count);
            if (!BatchFitsHeap(slots, count, *maxIt, _capacity, "Resource")) {
                return;
            }
            flushOffset               = static_cast<VkDeviceSize>(*minIt) * _stride;
            flushSize                 = (static_cast<VkDeviceSize>(*maxIt) + 1U) * _stride - flushOffset;
            // vkFlushMappedMemoryRanges requires offsets/sizes aligned to
            // nonCoherentAtomSize; round down the offset and extend the size.
            flushOffset = AlignDown(flushOffset, _nonCoherentAtomSize);
            flushSize   = AlignUp(flushSize, _nonCoherentAtomSize);
        }
        batch.Flush(_device, _vkWriteDescriptorsEXT, _mappedPtr, _stride);
        if (flushSize > 0) {
            FlushHostCache(flushOffset, flushSize);
        }
    }
}

template <DescriptorHeapType Type>
void DescriptorHeap<Type>::Flush(SamplerWriteBatch& batch) noexcept
    requires(Type == DescriptorHeapType::Sampler)
{
    if (Valid() && _vkWriteDescriptorsEXT != nullptr) {
        const auto  count = batch.SlotCount();
        const auto* slots = batch.SlotsData();
        VkDeviceSize flushOffset = 0;
        VkDeviceSize flushSize   = 0;
        if (count > 0 && slots != nullptr && _stride > 0) {
            const auto [minIt, maxIt] = std::minmax_element(slots, slots + count);
            if (!BatchFitsHeap(slots, count, *maxIt, _capacity, "Sampler")) {
                return;
            }
            flushOffset               = static_cast<VkDeviceSize>(*minIt) * _stride;
            flushSize                 = (static_cast<VkDeviceSize>(*maxIt) + 1U) * _stride - flushOffset;
            flushOffset = AlignDown(flushOffset, _nonCoherentAtomSize);
            flushSize   = AlignUp(flushSize, _nonCoherentAtomSize);
        }
        batch.Flush(_device, _vkWriteDescriptorsEXT, _mappedPtr, _stride);
        if (flushSize > 0) {
            FlushHostCache(flushOffset, flushSize);
        }
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

auto ResourceWriteBatch::Empty() const noexcept -> bool {
    return _impl->slots.empty();
}

auto ResourceWriteBatch::SlotCount() const noexcept -> uint32_t {
    return static_cast<uint32_t>(_impl->slots.size());
}

auto ResourceWriteBatch::SlotsData() const noexcept -> const uint32_t* {
    return _impl->slots.data();
}

void ResourceWriteBatch::AddImage(TextureHandle handle, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout) noexcept {
    // 1. Store the structure by value to keep it alive until Flush() completes
    _impl->viewInfos.push_back(viewInfo);

    // 2. Queue with pView set to nullptr for now. We will resolve stable memory pointers inside Flush()!
    _impl->imageInfos.push_back({.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT, .pNext = nullptr, .pView = nullptr, .layout = layout});
    _impl->slots.push_back(handle.index);
    _impl->types.push_back(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
}

void ResourceWriteBatch::AddStorageImage(StorageImageHandle handle, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout) noexcept {
    // 1. Store the structure by value to keep it alive until Flush() completes
    _impl->viewInfos.push_back(viewInfo);

    // 2. Queue with pView set to nullptr for now. We will resolve stable memory pointers inside Flush()!
    _impl->imageInfos.push_back({.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT, .pNext = nullptr, .pView = nullptr, .layout = layout});
    _impl->slots.push_back(handle.index);
    _impl->types.push_back(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
}

void ResourceWriteBatch::AddBuffer(StorageBufferHandle handle, VkDeviceAddress address, VkDeviceSize size) noexcept {
    _impl->addressRanges.push_back({.address = address, .size = size});
    _impl->slots.push_back(handle.index);
    _impl->types.push_back(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
}

void ResourceWriteBatch::AddBuffer(UniformBufferHandle handle, VkDeviceAddress address, VkDeviceSize size) noexcept {
    _impl->addressRanges.push_back({.address = address, .size = size});
    _impl->slots.push_back(handle.index);
    _impl->types.push_back(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
}

void ResourceWriteBatch::AddAccelerationStructure(AccelerationStructureHandle handle, VkDeviceAddress address) noexcept {
    // Acceleration structure descriptors ignore the range size (only the
    // 256-byte address alignment is validated), so a zero size is safe.
    _impl->addressRanges.push_back({.address = address, .size = 0});
    _impl->slots.push_back(handle.index);
    _impl->types.push_back(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
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

        if (_impl->types[i] == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || _impl->types[i] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
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

auto SamplerWriteBatch::Empty() const noexcept -> bool {
    return _impl->slots.empty();
}

auto SamplerWriteBatch::SlotCount() const noexcept -> uint32_t {
    return static_cast<uint32_t>(_impl->slots.size());
}

auto SamplerWriteBatch::SlotsData() const noexcept -> const uint32_t* {
    return _impl->slots.data();
}

void SamplerWriteBatch::AddSampler(SamplerHandle handle, const VkSamplerCreateInfo& createInfo) noexcept {
    _impl->createInfos.push_back(createInfo);
    _impl->slots.push_back(handle.index);
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
    Error                 errorOnExhaustion {DescriptorHeapError::ResourceSlotsExhausted};
};

SlotAllocator::SlotAllocator() noexcept: _impl(std::make_unique<Impl>()) {
}
SlotAllocator::~SlotAllocator() noexcept = default;

SlotAllocator::SlotAllocator(SlotAllocator&& other) noexcept                    = default;
auto SlotAllocator::operator=(SlotAllocator&& other) noexcept -> SlotAllocator& = default;

void SlotAllocator::Init(uint32_t capacity, Error errorOnExhaustion) noexcept {
    _impl->capacity = capacity;
    _impl->nextSlot = 0;
    _impl->freeSlots.clear();
    _impl->errorOnExhaustion = errorOnExhaustion;
}

auto SlotAllocator::Allocate() noexcept -> std::expected<uint32_t, Error> {
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

void SlotAllocator::Skip(uint32_t count) noexcept {
    _impl->nextSlot = _impl->nextSlot + count > _impl->capacity ? _impl->capacity : _impl->nextSlot + count;
}

auto SlotAllocator::Cursor() const noexcept -> uint32_t {
    return _impl->nextSlot;
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
) noexcept -> std::expected<void, Error> {
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

    // Capture push-data budget (vkCmdPushDataEXT limit) for the engine.
    VkPhysicalDeviceDescriptorHeapPropertiesEXT props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT,
        .pNext = nullptr,
    };
    VkPhysicalDeviceProperties2 props2 = {
        .sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext      = &props,
        .properties = {},
    };
    vkGetPhysicalDeviceProperties2(ctx.Physical(), &props2);
    _maxPushDataSize = props.maxPushDataSize;

    return {};
}

void HeapManager::BeginFrame(uint32_t frameIndex) noexcept {
    _currentFrameIndex        = frameIndex % _doubleBufferCount;
    _dynamicResourceAllocated = 0;
    _dynamicSamplerAllocated  = 0;
}

auto HeapManager::AllocateStaticResourceSlot() noexcept -> std::expected<uint32_t, Error> {
    return _staticResourceAlloc.Allocate();
}

void HeapManager::FreeStaticResourceSlot(uint32_t slot) noexcept {
    _staticResourceAlloc.Free(slot);
}

auto HeapManager::AllocateStaticSamplerSlot() noexcept -> std::expected<uint32_t, Error> {
    return _staticSamplerAlloc.Allocate();
}

void HeapManager::FreeStaticSamplerSlot(uint32_t slot) noexcept {
    _staticSamplerAlloc.Free(slot);
}

auto HeapManager::AllocateDynamicResourceRangeSlot(uint32_t count) noexcept -> std::expected<uint32_t, Error> {
    const uint32_t base_slot = _staticResourceCount + (_currentFrameIndex * _dynamicResourceCount) + _dynamicResourceAllocated;
    if (_dynamicResourceAllocated + count > _dynamicResourceCount) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::DynamicResourceOverflow);
    }
    _dynamicResourceAllocated += count;
    return base_slot;
}

auto HeapManager::AllocateDynamicSamplerRangeSlot(uint32_t count) noexcept -> std::expected<uint32_t, Error> {
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

auto HeapManager::StaticResourceCursor() const noexcept -> uint32_t {
    return _staticResourceAlloc.Cursor();
}

auto HeapManager::StaticSamplerCursor() const noexcept -> uint32_t {
    return _staticSamplerAlloc.Cursor();
}

// ============================================================================
// Host-Side Descriptor Writes
// ============================================================================

void HeapManager::WriteImage(TextureHandle handle, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout) noexcept {
    if (!handle.Valid()) {
        return;
    }
    ResourceWriteBatch batch;
    batch.AddImage(handle, viewInfo, layout);
    FlushResourceBatch(batch);
}

void HeapManager::WriteStorageImage(StorageImageHandle handle, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout) noexcept {
    if (!handle.Valid()) {
        return;
    }
    ResourceWriteBatch batch;
    batch.AddStorageImage(handle, viewInfo, layout);
    FlushResourceBatch(batch);
}

void HeapManager::WriteBuffer(StorageBufferHandle handle, VkDeviceAddress address, VkDeviceSize size) noexcept {
    if (!handle.Valid()) {
        return;
    }
    ResourceWriteBatch batch;
    batch.AddBuffer(handle, address, size);
    FlushResourceBatch(batch);
}

void HeapManager::WriteBuffer(UniformBufferHandle handle, VkDeviceAddress address, VkDeviceSize size) noexcept {
    if (!handle.Valid()) {
        return;
    }
    ResourceWriteBatch batch;
    batch.AddBuffer(handle, address, size);
    FlushResourceBatch(batch);
}

void HeapManager::WriteAccelerationStructure(AccelerationStructureHandle handle, VkDeviceAddress address) noexcept {
    if (!handle.Valid()) {
        return;
    }
    ResourceWriteBatch batch;
    batch.AddAccelerationStructure(handle, address);
    FlushResourceBatch(batch);
}

void HeapManager::WriteSampler(SamplerHandle handle, const VkSamplerCreateInfo& createInfo) noexcept {
    if (!handle.Valid()) {
        return;
    }
    SamplerWriteBatch batch;
    batch.AddSampler(handle, createInfo);
    FlushSamplerBatch(batch);
}

// Explicit template instantiations for class-level compilation protection
template class DescriptorHeap<DescriptorHeapType::Resource>;
template class DescriptorHeap<DescriptorHeapType::Sampler>;

} // namespace ZHLN::Vk
