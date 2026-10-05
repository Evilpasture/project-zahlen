// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DescriptorHeap.hpp"
#include "Rendering.hpp"
#include "memory/Allocator.hpp"
#include <Zahlen/Core/Defer.hpp>
#include <Zahlen/Core/Math.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <memory_resource>
#include <utility>
#include <vector>

namespace ZHLN::Vk {

namespace {

[[nodiscard]] auto BatchFitsHeap(uint32_t maxSlot, uint32_t capacity, uint32_t count, const char* heapName) noexcept -> bool {
    if (maxSlot < capacity) {
        return true;
    }
    ZHLN::Log("[DescriptorHeap] {} heap write out of range: slot {} >= capacity {}. Dropping {} descriptor write(s).", heapName, maxSlot, capacity, count);
    return false;
}

} // namespace

template <DescriptorHeapType Type>
DescriptorHeap<Type>::~DescriptorHeap() noexcept {
    Cleanup();
}

template <DescriptorHeapType Type>
DescriptorHeap<Type>::DescriptorHeap(DescriptorHeap&& other) noexcept:
    _device(std::exchange(other._device, VK_NULL_HANDLE)), _capacity(std::exchange(other._capacity, 0)), _stride(std::exchange(other._stride, 0)),
    _reservedSize(std::exchange(other._reservedSize, 0)), _allocator(std::exchange(other._allocator, nullptr)), _buffer(std::move(other._buffer)),
    _mappedRegion(std::move(other._mappedRegion)), _mappedPtr(std::exchange(other._mappedPtr, nullptr)),
    _bindInfo(std::exchange(other._bindInfo, VkBindHeapInfoEXT {})) {
}

template <DescriptorHeapType Type>
auto DescriptorHeap<Type>::operator=(DescriptorHeap&& other) noexcept -> DescriptorHeap& {
    if (this != &other) {
        Cleanup();
        _device       = std::exchange(other._device, VK_NULL_HANDLE);
        _capacity     = std::exchange(other._capacity, 0);
        _stride       = std::exchange(other._stride, 0);
        _reservedSize = std::exchange(other._reservedSize, 0);
        _allocator    = std::exchange(other._allocator, nullptr);
        _buffer       = std::move(other._buffer);
        _mappedRegion = std::move(other._mappedRegion);
        _mappedPtr    = std::exchange(other._mappedPtr, nullptr);
        _bindInfo     = std::exchange(other._bindInfo, VkBindHeapInfoEXT {});
    }
    return *this;
}

template <DescriptorHeapType Type>
void DescriptorHeap<Type>::Cleanup() noexcept {
    _mappedRegion = {};
    if (_allocator != nullptr) {
        _allocator->DestroyBuffer(_buffer);
    }
    _allocator    = nullptr;
    _mappedPtr    = nullptr;
    _capacity     = 0;
    _stride       = 0;
    _reservedSize = 0;
    _bindInfo     = {};
}

template <DescriptorHeapType Type>
auto DescriptorHeap<Type>::Init(const Context& ctx, Allocator& allocator, uint32_t capacity) noexcept -> std::expected<void, ErrorCode> {
    Cleanup();
    _device    = ctx.Device();
    _allocator = &allocator;
    _capacity  = capacity;
    ZHLN::defer rollback([this] { Cleanup(); });

    if (!ctx.DescriptorHeapsSupported()) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::ExtensionUnavailable);
    }

    if constexpr (Type == DescriptorHeapType::Samplers) {
        if (vkCmdBindSamplerHeapEXT == nullptr || vkWriteSamplerDescriptorsEXT == nullptr) [[unlikely]] {
            return std::unexpected(DescriptorHeapError::FunctionLoaderFailed);
        }
    } else {
        if (vkCmdBindResourceHeapEXT == nullptr || vkWriteResourceDescriptorsEXT == nullptr) [[unlikely]] {
            return std::unexpected(DescriptorHeapError::FunctionLoaderFailed);
        }
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
    if constexpr (Type == DescriptorHeapType::Samplers) {
        _stride        = ZHLN::Math::AlignUp(props.samplerDescriptorSize, props.samplerDescriptorAlignment);
        _reservedSize  = props.minSamplerHeapReservedRange;
        heap_alignment = props.samplerHeapAlignment;
        max_heap_size  = props.maxSamplerHeapSize;
    } else {
        const VkDeviceSize max_size  = std::max(props.bufferDescriptorSize, props.imageDescriptorSize);
        const VkDeviceSize max_align = std::max(props.bufferDescriptorAlignment, props.imageDescriptorAlignment);
        _stride                      = ZHLN::Math::AlignUp(max_size, max_align);
        _reservedSize                = props.minResourceHeapReservedRange;
        heap_alignment               = props.resourceHeapAlignment;
        max_heap_size                = props.maxResourceHeapSize;
    }

    if (_stride == 0) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::ExtensionUnavailable);
    }

    const VkDeviceSize used_bytes  = _stride * _capacity;
    const VkDeviceSize total_bytes = ZHLN::Math::AlignUp(used_bytes + _reservedSize, std::max<VkDeviceSize>(heap_alignment, 1));

    if (total_bytes > max_heap_size) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::HeapTooLarge);
    }

    auto buffer_res = Buffer::Create(
        allocator, total_bytes, BufferUsage::DescriptorHeap | BufferUsage::ShaderDeviceAddress, MemoryUsage::CPUToGPU, std::max<VkDeviceSize>(heap_alignment, 1)
    );

    if (!buffer_res.has_value()) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::AllocationFailed);
    }
    _buffer = std::move(*buffer_res);

    auto mapped = _buffer.Map(*_allocator);
    if (!mapped) [[unlikely]] {
        return std::unexpected(mapped.error());
    }
    _mappedRegion = std::move(*mapped);
    _mappedPtr    = _mappedRegion.Data();

    const VkDeviceAddress address = GetBufferAddress(_device, _buffer.Handle());
    if (address == 0) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::DeviceAddressFailed);
    }

    if ((address % std::max<VkDeviceSize>(heap_alignment, 1)) != 0) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::DeviceAddressFailed);
    }

    _bindInfo = {
        .sType               = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .pNext               = nullptr,
        .heapRange           = {.address = address, .size = total_bytes},
        .reservedRangeOffset = used_bytes,
        .reservedRangeSize   = _reservedSize,
    };

    rollback.Dismiss();
    return {};
}

template <DescriptorHeapType Type>
void DescriptorHeap<Type>::FlushHostCache(VkDeviceSize offset, VkDeviceSize size) noexcept {
    if (_allocator != nullptr) {
        _buffer.Flush(*_allocator, offset, size);
    }
}

template <DescriptorHeapType Type>
void DescriptorHeap<Type>::Bind(VkCommandBuffer cmd) const noexcept {
    if (!Valid()) {
        return;
    }
    if constexpr (Type == DescriptorHeapType::Samplers) {
        vkCmdBindSamplerHeapEXT(cmd, &_bindInfo);
    } else {
        vkCmdBindResourceHeapEXT(cmd, &_bindInfo);
    }
}

template <DescriptorHeapType Type>
void DescriptorHeap<Type>::Flush(ResourceWriteBatch& batch) noexcept
    requires(Type == DescriptorHeapType::Resources)
{
    if (Valid() && vkWriteResourceDescriptorsEXT != nullptr) {
        const auto   count       = batch.SlotCount();
        VkDeviceSize flushOffset = 0;
        VkDeviceSize flushSize   = 0;
        if (count > 0 && _stride > 0) {
            const auto [minSlot, maxSlot] = batch.SlotBounds();
            if (!BatchFitsHeap(maxSlot, _capacity, count, "Resource")) {
                return;
            }
            flushOffset = static_cast<VkDeviceSize>(minSlot) * _stride;
            flushSize   = (static_cast<VkDeviceSize>(maxSlot) + 1U) * _stride - flushOffset;
            flushOffset = ZHLN::Math::AlignDown(flushOffset, _nonCoherentAtomSize);
            flushSize   = ZHLN::Math::AlignUp(flushSize, _nonCoherentAtomSize);
        }
        batch.Flush(_device, _mappedPtr, _stride);
        if (flushSize > 0) {
            FlushHostCache(flushOffset, flushSize);
        }
    }
}

template <DescriptorHeapType Type>
void DescriptorHeap<Type>::Flush(SamplerWriteBatch& batch) noexcept
    requires(Type == DescriptorHeapType::Samplers)
{
    if (Valid() && vkWriteSamplerDescriptorsEXT != nullptr) {
        const auto   count       = batch.SlotCount();
        const auto*  slots       = batch.SlotsData();
        VkDeviceSize flushOffset = 0;
        VkDeviceSize flushSize   = 0;
        if (count > 0 && slots != nullptr && _stride > 0) {
            const auto [minIt, maxIt] = std::minmax_element(slots, slots + count);
            if (!BatchFitsHeap(*maxIt, _capacity, count, "Sampler")) {
                return;
            }
            flushOffset = static_cast<VkDeviceSize>(*minIt) * _stride;
            flushSize   = (static_cast<VkDeviceSize>(*maxIt) + 1U) * _stride - flushOffset;
            flushOffset = ZHLN::Math::AlignDown(flushOffset, _nonCoherentAtomSize);
            flushSize   = ZHLN::Math::AlignUp(flushSize, _nonCoherentAtomSize);
        }
        batch.Flush(_device, _mappedPtr, _stride);
        if (flushSize > 0) {
            FlushHostCache(flushOffset, flushSize);
        }
    }
}

struct ResourceWriteBatch::Impl {
    // pView points into this payload, which lives at a fixed address in the
    // batch arena until the synchronous vkWriteResourceDescriptorsEXT call.
    struct ImagePayload {
        VkImageViewCreateInfo    viewInfo {};
        VkImageDescriptorInfoEXT descriptor {};

        ImagePayload(const VkImageViewCreateInfo& info, VkImageLayout layout) noexcept:
            viewInfo(info), descriptor {.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT, .pNext = nullptr, .pView = &viewInfo, .layout = layout} {
        }
        ImagePayload(const ImagePayload&)                    = delete;
        auto operator=(const ImagePayload&) -> ImagePayload& = delete;
        ImagePayload(ImagePayload&&)                         = delete;
        auto operator=(ImagePayload&&) -> ImagePayload&      = delete;
    };

    struct Write {
        uint32_t                    slot = 0;
        VkResourceDescriptorInfoEXT descriptor {};
    };

    alignas(std::max_align_t) std::array<std::byte, 1024> storage {};
    std::pmr::monotonic_buffer_resource arena {storage.data(), storage.size()};
    std::vector<Write>                  writes;

    void AddImage(uint32_t slot, VkDescriptorType type, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout) {
        std::pmr::polymorphic_allocator<ImagePayload> alloc {&arena};
        auto*                                         image = alloc.new_object<ImagePayload>(viewInfo, layout);

        VkResourceDescriptorInfoEXT descriptor {.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT, .pNext = nullptr, .type = type, .data = {}};
        descriptor.data.pImage = &image->descriptor;
        writes.push_back({.slot = slot, .descriptor = descriptor});
    }

    void AddAddress(uint32_t slot, VkDescriptorType type, VkDeviceAddressRangeEXT range) {
        std::pmr::polymorphic_allocator<VkDeviceAddressRangeEXT> alloc {&arena};
        auto*                                                    address = alloc.new_object<VkDeviceAddressRangeEXT>(range);

        VkResourceDescriptorInfoEXT descriptor {.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT, .pNext = nullptr, .type = type, .data = {}};
        descriptor.data.pAddressRange = address;
        writes.push_back({.slot = slot, .descriptor = descriptor});
    }
};

ResourceWriteBatch::ResourceWriteBatch() noexcept: _impl(std::make_unique<Impl>()) {
}
ResourceWriteBatch::~ResourceWriteBatch() noexcept = default;

ResourceWriteBatch::ResourceWriteBatch(ResourceWriteBatch&& other) noexcept                    = default;
auto ResourceWriteBatch::operator=(ResourceWriteBatch&& other) noexcept -> ResourceWriteBatch& = default;

auto ResourceWriteBatch::Empty() const noexcept -> bool {
    return _impl->writes.empty();
}

auto ResourceWriteBatch::SlotCount() const noexcept -> uint32_t {
    return static_cast<uint32_t>(_impl->writes.size());
}

auto ResourceWriteBatch::SlotBounds() const noexcept -> std::pair<uint32_t, uint32_t> {
    if (_impl->writes.empty()) {
        return {0, 0};
    }
    uint32_t minSlot = _impl->writes.front().slot;
    uint32_t maxSlot = minSlot;
    for (const auto& write: _impl->writes) {
        minSlot = std::min(minSlot, write.slot);
        maxSlot = std::max(maxSlot, write.slot);
    }
    return {minSlot, maxSlot};
}

void ResourceWriteBatch::AddImage(TextureHandle handle, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout) noexcept {
    _impl->AddImage(handle.index, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, viewInfo, layout);
}

void ResourceWriteBatch::AddStorageImage(StorageImageHandle handle, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout) noexcept {
    _impl->AddImage(handle.index, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, viewInfo, layout);
}

void ResourceWriteBatch::AddBuffer(StorageBufferHandle handle, BufferSlice slice) noexcept {
    _impl->AddAddress(handle.index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, {.address = slice.Address(), .size = slice.Size()});
}

void ResourceWriteBatch::AddBuffer(UniformBufferHandle handle, BufferSlice slice) noexcept {
    _impl->AddAddress(handle.index, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, {.address = slice.Address(), .size = slice.Size()});
}

void ResourceWriteBatch::AddAccelerationStructure(AccelerationStructureHandle handle, VkDeviceAddress address) noexcept {
    _impl->AddAddress(handle.index, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, {.address = address, .size = 0});
}

void ResourceWriteBatch::Flush(VkDevice device, void* mappedPtr, VkDeviceSize stride) noexcept {
    const auto total_count = static_cast<uint32_t>(_impl->writes.size());
    if (total_count == 0) {
        return;
    }

    // Only the API's two flat arrays are assembled here. Every nested pointer
    // already targets stable arena storage, regardless of writes vector growth.
    std::vector<VkResourceDescriptorInfoEXT> resource_infos;
    std::vector<VkHostAddressRangeEXT>       ranges;
    resource_infos.reserve(total_count);
    ranges.reserve(total_count);

    for (const auto& write: _impl->writes) {
        resource_infos.push_back(write.descriptor);
        ranges.push_back({.address = static_cast<uint8_t*>(mappedPtr) + (write.slot * stride), .size = stride});
    }

    vkWriteResourceDescriptorsEXT(device, total_count, resource_infos.data(), ranges.data());

    _impl->writes.clear();
    _impl->arena.release();
}

struct SamplerWriteBatch::Impl {
    std::vector<VkSamplerCreateInfo> createInfos;
    std::vector<uint32_t>            slots;
};

SamplerWriteBatch::SamplerWriteBatch() noexcept: _impl(std::make_unique<Impl>()) {
}
SamplerWriteBatch::~SamplerWriteBatch() noexcept = default;

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

void SamplerWriteBatch::Flush(VkDevice device, void* mappedPtr, VkDeviceSize stride) noexcept {
    const auto total_count = static_cast<uint32_t>(_impl->slots.size());
    if (total_count == 0) {
        return;
    }

    std::vector<VkHostAddressRangeEXT> ranges(total_count);
    for (uint32_t i = 0; i < total_count; ++i) {
        ranges[i] = {.address = static_cast<uint8_t*>(mappedPtr) + (_impl->slots[i] * stride), .size = stride};
    }

    vkWriteSamplerDescriptorsEXT(device, total_count, _impl->createInfos.data(), ranges.data());

    _impl->createInfos.clear();
    _impl->slots.clear();
}

struct SlotAllocator::Impl {
    uint32_t              capacity = 0;
    uint32_t              nextSlot = 0;
    std::vector<uint32_t> freeSlots;
    ErrorCode             errorOnExhaustion {DescriptorHeapError::ResourceSlotsExhausted};
};

SlotAllocator::SlotAllocator() noexcept: _impl(std::make_unique<Impl>()) {
}
SlotAllocator::~SlotAllocator() noexcept = default;

SlotAllocator::SlotAllocator(SlotAllocator&& other) noexcept                    = default;
auto SlotAllocator::operator=(SlotAllocator&& other) noexcept -> SlotAllocator& = default;

void SlotAllocator::Init(uint32_t capacity, ErrorCode errorOnExhaustion) noexcept {
    _impl->capacity = capacity;
    _impl->nextSlot = 0;
    _impl->freeSlots.clear();
    _impl->errorOnExhaustion = errorOnExhaustion;
}

auto SlotAllocator::Allocate() noexcept -> std::expected<uint32_t, ErrorCode> {
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

auto HeapManager::Init(
    const Context& ctx,
    Allocator&     allocator,
    uint32_t       staticResourceCount,
    uint32_t       staticSamplerCount,
    uint32_t       frameTransientResourceCount,
    uint32_t       immediateTransientResourceCount
) noexcept -> std::expected<void, ErrorCode> {
    _staticResourceCount             = staticResourceCount;
    _staticSamplerCount              = staticSamplerCount;
    _frameTransientResourceCount     = frameTransientResourceCount;
    _immediateTransientResourceCount = immediateTransientResourceCount;
    _currentFrameIndex               = 0;

    _staticResourceAlloc.Init(staticResourceCount, DescriptorHeapError::ResourceSlotsExhausted);
    _staticSamplerAlloc.Init(staticSamplerCount, DescriptorHeapError::SamplerSlotsExhausted);

    const uint32_t total_resource_count = staticResourceCount + (kFramesInFlight * frameTransientResourceCount) + immediateTransientResourceCount;
    const uint32_t total_sampler_count  = staticSamplerCount;

    auto res_heap_init = _resourceHeap.Init(ctx, allocator, total_resource_count);
    if (!res_heap_init.has_value()) [[unlikely]] {
        return std::unexpected(res_heap_init.error());
    }

    auto samp_heap_init = _samplerHeap.Init(ctx, allocator, total_sampler_count);
    if (!samp_heap_init.has_value()) [[unlikely]] {
        return std::unexpected(samp_heap_init.error());
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
    _maxPushDataSize = props.maxPushDataSize;

    return {};
}

void HeapManager::BeginFrame(uint32_t frameIndex) noexcept {
    if constexpr (isDev) {
        if (_frameTransientResourceCount > 0 && _frameTransientAllocated * 4 > _frameTransientResourceCount * 3) [[unlikely]] {
            ZHLN::Log(
                "[VK_EXT_descriptor_heap] frame transient partition {}% full ({} of {} slots); raise kFrameTransientResourceSlots.",
                (_frameTransientAllocated * 100) / _frameTransientResourceCount, _frameTransientAllocated, _frameTransientResourceCount
            );
        }
    }
    _currentFrameIndex       = FrameSlot(frameIndex);
    _frameTransientAllocated = 0;
}

void HeapManager::BeginImmediate() noexcept {
    _immediateTransientAllocated = 0;
}

auto HeapManager::AllocateStaticResourceSlot() noexcept -> std::expected<uint32_t, ErrorCode> {
    return _staticResourceAlloc.Allocate();
}

void HeapManager::FreeStaticResourceSlot(uint32_t slot) noexcept {
    _staticResourceAlloc.Free(slot);
}

auto HeapManager::AllocateStaticSamplerSlot() noexcept -> std::expected<uint32_t, ErrorCode> {
    return _staticSamplerAlloc.Allocate();
}

void HeapManager::FreeStaticSamplerSlot(uint32_t slot) noexcept {
    _staticSamplerAlloc.Free(slot);
}

auto HeapManager::AllocateTransientResourceRange(uint32_t count, HeapLifecycle lifecycle) noexcept -> std::expected<uint32_t, ErrorCode> {
    const ZHLN::MutexGuard guard(_writeMutex);

    if (lifecycle == HeapLifecycle::Immediate) {
        const uint32_t base_slot = _staticResourceCount + (kFramesInFlight * _frameTransientResourceCount) + _immediateTransientAllocated;
        if (_immediateTransientAllocated + count > _immediateTransientResourceCount) [[unlikely]] {
            return std::unexpected(DescriptorHeapError::TransientResourceOverflow);
        }
        _immediateTransientAllocated += count;
        return base_slot;
    }

    const uint32_t base_slot = _staticResourceCount + (_currentFrameIndex * _frameTransientResourceCount) + _frameTransientAllocated;
    if (_frameTransientAllocated + count > _frameTransientResourceCount) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::TransientResourceOverflow);
    }
    _frameTransientAllocated += count;
    return base_slot;
}

void HeapManager::FlushResourceBatch(ResourceWriteBatch& batch) noexcept {
    const ZHLN::MutexGuard guard(_writeMutex);
    _resourceHeap.Flush(batch);
}

void HeapManager::FlushSamplerBatch(SamplerWriteBatch& batch) noexcept {
    const ZHLN::MutexGuard guard(_writeMutex);
    _samplerHeap.Flush(batch);
}

void HeapManager::BindHeaps(VkCommandBuffer cmd) const noexcept {
    _resourceHeap.Bind(cmd);
    _samplerHeap.Bind(cmd);
}

auto HeapManager::ReserveOffsetAddressedResourceRegion(uint32_t count) noexcept -> std::expected<uint32_t, ErrorCode> {
    const uint32_t base = _staticResourceAlloc.Cursor();
    if (count > _staticResourceCount - base) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::ResourceSlotsExhausted);
    }
    _staticResourceAlloc.Skip(count);
    return base;
}

auto HeapManager::ReserveOffsetAddressedSamplerRegion(uint32_t count) noexcept -> std::expected<uint32_t, ErrorCode> {
    const uint32_t base = _staticSamplerAlloc.Cursor();
    if (count > _staticSamplerCount - base) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::SamplerSlotsExhausted);
    }
    _staticSamplerAlloc.Skip(count);
    return base;
}

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

void HeapManager::WriteBuffer(StorageBufferHandle handle, BufferSlice slice) noexcept {
    if (!handle.Valid()) {
        return;
    }
    ResourceWriteBatch batch;
    batch.AddBuffer(handle, slice);
    FlushResourceBatch(batch);
}

void HeapManager::WriteBuffer(UniformBufferHandle handle, BufferSlice slice) noexcept {
    if (!handle.Valid()) {
        return;
    }
    ResourceWriteBatch batch;
    batch.AddBuffer(handle, slice);
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

template class DescriptorHeap<DescriptorHeapType::Resources>;
template class DescriptorHeap<DescriptorHeapType::Samplers>;

} // namespace ZHLN::Vk
