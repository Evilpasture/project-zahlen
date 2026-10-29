// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DescriptorHeap.hpp"
#include "Rendering.hpp"
#include <Zahlen/Core/Defer.hpp>
#include <Zahlen/Core/Math.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace ZHLN::Vk {

namespace {

// How many descriptors a write synthesizes on the stack at a time. Both
// ResourceWriteBatch and SamplerWriteBatch hold compact engine value types;
// the 80-byte VkSamplerCreateInfo / VkImageViewCreateInfo C ABI structs exist
// only inside this fixed stack window during vkWrite*DescriptorsEXT.
inline constexpr uint32_t kDescriptorWriteChunk = 16;

[[nodiscard]] constexpr auto HeapTypeName(DescriptorHeapType type) noexcept -> const char* {
    return type == DescriptorHeapType::Samplers ? "Sampler" : "Resource";
}

} // namespace

// =============================================================================
// DescriptorHeap
// =============================================================================

DescriptorHeap::~DescriptorHeap() noexcept {
    Cleanup();
}

DescriptorHeap::DescriptorHeap(DescriptorHeap&& other) noexcept:
    _device(std::exchange(other._device, VK_NULL_HANDLE)),
    _type(other._type),
    _config(std::exchange(other._config, HeapConfig {})),
    _capacity(std::exchange(other._capacity, 0)),
    _staticCapacity(std::exchange(other._staticCapacity, 0)),
    _staticCursor(std::exchange(other._staticCursor, 0)),
    _frameTransientCapacity(std::exchange(other._frameTransientCapacity, 0)),
    _currentFrameSlot(std::exchange(other._currentFrameSlot, 0)),
    _frameTransientAllocated(std::exchange(other._frameTransientAllocated, 0)),
    _immediateTransientCapacity(std::exchange(other._immediateTransientCapacity, 0)),
    _immediateTransientAllocated(0),
    _stride(std::exchange(other._stride, 0)),
    _reservedSize(std::exchange(other._reservedSize, 0)),
    _nonCoherentAtomSize(std::exchange(other._nonCoherentAtomSize, 1)),
    _allocator(std::exchange(other._allocator, nullptr)),
    _buffer(std::move(other._buffer)),
    _mappedRegion(std::move(other._mappedRegion)),
    _mappedPtr(std::exchange(other._mappedPtr, nullptr)),
    _bindInfo(std::exchange(other._bindInfo, VkBindHeapInfoEXT {})),
    _nextImmediateScopeId(other._nextImmediateScopeId),
    _freeStaticSlots(std::move(other._freeStaticSlots)),
    _liveStaticBits(std::move(other._liveStaticBits)) {
    other._immediateTransientAllocated = 0;
    other._immediateScopes.clear();
    other._freeStaticSlots.clear();
    other._liveStaticBits.clear();
}

auto DescriptorHeap::operator=(DescriptorHeap&& other) noexcept -> DescriptorHeap& {
    if (this != &other) {
        Cleanup();
        _device                      = std::exchange(other._device, VK_NULL_HANDLE);
        _type                        = other._type;
        _config                      = std::exchange(other._config, HeapConfig {});
        _capacity                    = std::exchange(other._capacity, 0);
        _staticCapacity              = std::exchange(other._staticCapacity, 0);
        _staticCursor                = std::exchange(other._staticCursor, 0);
        _frameTransientCapacity      = std::exchange(other._frameTransientCapacity, 0);
        _currentFrameSlot            = std::exchange(other._currentFrameSlot, 0);
        _frameTransientAllocated     = std::exchange(other._frameTransientAllocated, 0);
        _immediateTransientCapacity  = std::exchange(other._immediateTransientCapacity, 0);
        _immediateTransientAllocated = 0;
        _stride                      = std::exchange(other._stride, 0);
        _reservedSize                = std::exchange(other._reservedSize, 0);
        _nonCoherentAtomSize         = std::exchange(other._nonCoherentAtomSize, 1);
        _allocator                   = std::exchange(other._allocator, nullptr);
        _buffer                      = std::move(other._buffer);
        _mappedRegion                = std::move(other._mappedRegion);
        _mappedPtr                   = std::exchange(other._mappedPtr, nullptr);
        _bindInfo                    = std::exchange(other._bindInfo, VkBindHeapInfoEXT {});
        _nextImmediateScopeId        = std::max(_nextImmediateScopeId, other._nextImmediateScopeId);
        _freeStaticSlots             = std::move(other._freeStaticSlots);
        _liveStaticBits              = std::move(other._liveStaticBits);
        other._immediateTransientAllocated = 0;
        other._immediateScopes.clear();
        other._freeStaticSlots.clear();
        other._liveStaticBits.clear();
    }
    return *this;
}

void DescriptorHeap::Cleanup() noexcept {
    _mappedRegion = {};
    if (_allocator != nullptr) {
        _allocator->DestroyBuffer(_buffer);
    }
    _device                      = VK_NULL_HANDLE;
    _config                      = {};
    _allocator                   = nullptr;
    _mappedPtr                   = nullptr;
    _capacity                    = 0;
    _staticCapacity              = 0;
    _staticCursor                = 0;
    _frameTransientCapacity      = 0;
    _currentFrameSlot            = 0;
    _frameTransientAllocated     = 0;
    _immediateTransientCapacity  = 0;
    _immediateTransientAllocated = 0;
    _stride                      = 0;
    _reservedSize                = 0;
    _nonCoherentAtomSize         = 1;
    _bindInfo                    = {};
    _immediateScopes.clear();
    _freeStaticSlots.clear();
    _liveStaticBits.clear();
}

auto DescriptorHeap::Init(
    const Context& ctx, Allocator& allocator, DescriptorHeapType type, PartitionLayout layout, HeapConfig config
) noexcept -> std::expected<void, ErrorCode> {
    Cleanup();
    _device                     = ctx.Device();
    _type                       = type;
    _config                     = config;
    _allocator                  = &allocator;
    _staticCapacity             = layout.staticSlots;
    _frameTransientCapacity     = layout.frameTransientSlots;
    _immediateTransientCapacity = layout.immediateTransientSlots;
    _capacity                   = _staticCapacity + (kFramesInFlight * _frameTransientCapacity) + _immediateTransientCapacity;
    ZHLN::defer rollback([this] { Cleanup(); });

    if (!ctx.DescriptorHeapsSupported()) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::ExtensionUnavailable);
    }

    if (_type == DescriptorHeapType::Samplers) {
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
    _nonCoherentAtomSize = std::max<VkDeviceSize>(props2.properties.limits.nonCoherentAtomSize, 1);

    VkDeviceSize heapAlignment = 0;
    VkDeviceSize maxHeapSize   = 0;
    if (_type == DescriptorHeapType::Samplers) {
        _stride       = ZHLN::Math::AlignUp(props.samplerDescriptorSize, props.samplerDescriptorAlignment);
        _reservedSize = props.minSamplerHeapReservedRange;
        heapAlignment = props.samplerHeapAlignment;
        maxHeapSize   = props.maxSamplerHeapSize;
    } else {
        const VkDeviceSize maxSize  = std::max(props.bufferDescriptorSize, props.imageDescriptorSize);
        const VkDeviceSize maxAlign = std::max(props.bufferDescriptorAlignment, props.imageDescriptorAlignment);
        _stride                     = ZHLN::Math::AlignUp(maxSize, maxAlign);
        _reservedSize               = props.minResourceHeapReservedRange;
        heapAlignment               = props.resourceHeapAlignment;
        maxHeapSize                 = props.maxResourceHeapSize;
    }

    if (_stride == 0) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::ExtensionUnavailable);
    }

    const VkDeviceSize effectiveAlign = std::max<VkDeviceSize>(heapAlignment, 1);
    const VkDeviceSize usedBytes      = _stride * _capacity;
    const VkDeviceSize totalBytes     = ZHLN::Math::AlignUp(usedBytes + _reservedSize, effectiveAlign);

    if (totalBytes > maxHeapSize) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::HeapTooLarge);
    }

    auto bufferRes = Buffer::Create(
        allocator, totalBytes, BufferUsage::DescriptorHeap | BufferUsage::ShaderDeviceAddress, MemoryUsage::CPUToGPU, effectiveAlign
    );
    if (!bufferRes.has_value()) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::AllocationFailed);
    }
    _buffer = std::move(*bufferRes);
    Debug::SetBufferName(ctx, _buffer.Handle(), _type == DescriptorHeapType::Samplers ? "DescriptorHeap.Samplers" : "DescriptorHeap.Resources");

    auto mapped = _buffer.Map(*_allocator);
    if (!mapped) [[unlikely]] {
        return std::unexpected(mapped.error());
    }
    _mappedRegion = std::move(*mapped);
    _mappedPtr    = _mappedRegion.Data();

    const VkDeviceAddress address = ctx.BufferAddress(_buffer.Handle());
    if (address == 0 || (address % effectiveAlign) != 0) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::DeviceAddressFailed);
    }

    _liveStaticBits.assign((static_cast<size_t>(_staticCapacity) + 63U) / 64U, 0ULL);

    _bindInfo = {
        .sType               = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .pNext               = nullptr,
        .heapRange           = {.address = address, .size = totalBytes},
        .reservedRangeOffset = usedBytes,
        .reservedRangeSize   = _reservedSize,
    };

    rollback.Dismiss();
    return {};
}

void DescriptorHeap::Bind(VkCommandBuffer cmd) const noexcept {
    if (!Valid()) {
        return;
    }
    if (_type == DescriptorHeapType::Samplers) {
        vkCmdBindSamplerHeapEXT(cmd, &_bindInfo);
    } else {
        vkCmdBindResourceHeapEXT(cmd, &_bindInfo);
    }
}

void DescriptorHeap::MarkStaticLive(uint32_t slot) noexcept {
    if (slot < _staticCapacity) {
        _liveStaticBits[slot >> 6U] |= (uint64_t {1} << (slot & 63U));
    }
}

void DescriptorHeap::ClearStaticLive(uint32_t slot) noexcept {
    if (slot < _staticCapacity) {
        _liveStaticBits[slot >> 6U] &= ~(uint64_t {1} << (slot & 63U));
    }
}

auto DescriptorHeap::IsStaticLive(uint32_t slot) const noexcept -> bool {
    if (slot >= _staticCapacity) {
        return false;
    }
    return ((_liveStaticBits[slot >> 6U] >> (slot & 63U)) & uint64_t {1}) != 0;
}

auto DescriptorHeap::AllocateStatic() noexcept -> std::expected<uint32_t, ErrorCode> {
    if (!_freeStaticSlots.empty()) {
        const uint32_t slot = _freeStaticSlots.back();
        _freeStaticSlots.pop_back();
        MarkStaticLive(slot);
        return slot;
    }
    if (_staticCursor < _staticCapacity) {
        const uint32_t slot = _staticCursor++;
        MarkStaticLive(slot);
        return slot;
    }
    return std::unexpected(
        _type == DescriptorHeapType::Samplers ? DescriptorHeapError::SamplerSlotsExhausted : DescriptorHeapError::ResourceSlotsExhausted
    );
}

auto DescriptorHeap::AllocateStaticRange(uint32_t count) noexcept -> std::expected<uint32_t, ErrorCode> {
    if (count > _staticCapacity - _staticCursor) [[unlikely]] {
        return std::unexpected(
            _type == DescriptorHeapType::Samplers ? DescriptorHeapError::SamplerSlotsExhausted : DescriptorHeapError::ResourceSlotsExhausted
        );
    }
    const uint32_t base = _staticCursor;
    _staticCursor += count;
    for (uint32_t i = 0; i < count; ++i) {
        MarkStaticLive(base + i);
    }
    return base;
}

void DescriptorHeap::FreeStatic(uint32_t slot) noexcept {
    if (slot < _staticCursor && IsStaticLive(slot)) {
        ClearStaticLive(slot);
        _freeStaticSlots.push_back(slot);
    }
}

void DescriptorHeap::BeginFrame(uint32_t frameIndex) noexcept {
    if (_config.enableCapacityWarnings && _frameTransientCapacity > 0) {
        const uint64_t threshold = std::min<uint64_t>(_config.warningThresholdPercent, 100ULL);
        if (uint64_t {_frameTransientAllocated} * 100ULL > uint64_t {_frameTransientCapacity} * threshold) [[unlikely]] {
            ZHLN::Log(
                "[VK_EXT_descriptor_heap] frame transient partition {}% full ({} of {} slots); raise kFrameTransientResourceSlots.",
                (uint64_t {_frameTransientAllocated} * 100ULL) / uint64_t {_frameTransientCapacity}, _frameTransientAllocated, _frameTransientCapacity
            );
        }
    }
    _currentFrameSlot        = FrameSlot(frameIndex);
    _frameTransientAllocated = 0;
}

DescriptorHeap::ImmediateScope::~ImmediateScope() noexcept {
    Reset();
}

DescriptorHeap::ImmediateScope::ImmediateScope(ImmediateScope&& other) noexcept:
    _heap(std::exchange(other._heap, nullptr)), _scopeId(std::exchange(other._scopeId, 0)) {
}

auto DescriptorHeap::ImmediateScope::operator=(ImmediateScope&& other) noexcept -> ImmediateScope& {
    if (this != &other) {
        Reset();
        _heap    = std::exchange(other._heap, nullptr);
        _scopeId = std::exchange(other._scopeId, 0);
    }
    return *this;
}

void DescriptorHeap::ImmediateScope::Reset() noexcept {
    if (_heap != nullptr) {
        _heap->LeaveImmediate(_scopeId);
        _heap    = nullptr;
        _scopeId = 0;
    }
}

auto DescriptorHeap::CreateImmediateScope() noexcept -> ImmediateScope {
    return ImmediateScope {*this, EnterImmediate()};
}

auto DescriptorHeap::EnterImmediate() noexcept -> uint64_t {
    const uint64_t scopeId = ++_nextImmediateScopeId;
    _immediateScopes.push_back({
        .id         = scopeId,
        .baseCursor = _immediateTransientAllocated,
        .closed     = false,
    });
    return scopeId;
}

void DescriptorHeap::LeaveImmediate(uint64_t scopeId) noexcept {
    for (size_t i = _immediateScopes.size(); i > 0; --i) {
        auto& entry = _immediateScopes[i - 1];
        if (entry.id == scopeId) {
            entry.closed = true;
            break;
        }
    }

    uint32_t rewindCursor = _immediateTransientAllocated;
    while (!_immediateScopes.empty() && _immediateScopes.back().closed) {
        rewindCursor = _immediateScopes.back().baseCursor;
        _immediateScopes.pop_back();
    }
    if (rewindCursor < _immediateTransientAllocated) {
        _immediateTransientAllocated = rewindCursor;
    }
}

auto DescriptorHeap::AllocateTransientRange(uint32_t count, HeapLifecycle lifecycle) noexcept -> std::expected<uint32_t, ErrorCode> {
    if (lifecycle == HeapLifecycle::Immediate) {
        if (_immediateScopes.empty()) [[unlikely]] {
            return std::unexpected(DescriptorHeapError::NoActiveImmediateScope);
        }
        if (count > _immediateTransientCapacity - _immediateTransientAllocated) [[unlikely]] {
            return std::unexpected(DescriptorHeapError::TransientResourceOverflow);
        }
        const uint32_t baseSlot = _staticCapacity + (kFramesInFlight * _frameTransientCapacity) + _immediateTransientAllocated;
        _immediateTransientAllocated += count;
        return baseSlot;
    }

    if (count > _frameTransientCapacity - _frameTransientAllocated) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::TransientResourceOverflow);
    }
    const uint32_t baseSlot = _staticCapacity + (_currentFrameSlot * _frameTransientCapacity) + _frameTransientAllocated;
    _frameTransientAllocated += count;
    return baseSlot;
}

auto DescriptorHeap::IsSlotWritable(uint32_t slot) const noexcept -> bool {
    if (slot >= _capacity) {
        return false;
    }
    if (slot < _staticCapacity) {
        return slot < _staticCursor && IsStaticLive(slot);
    }

    const uint32_t immediateBase = _staticCapacity + (kFramesInFlight * _frameTransientCapacity);
    if (slot < immediateBase) {
        const uint32_t activeFrameBase = _staticCapacity + (_currentFrameSlot * _frameTransientCapacity);
        return slot >= activeFrameBase && (slot - activeFrameBase) < _frameTransientAllocated;
    }

    return (slot - immediateBase) < _immediateTransientAllocated;
}

auto DescriptorHeap::ValidateWriteSlot(uint32_t slot, uint32_t batchCount) const noexcept -> bool {
    if (slot >= _capacity) [[unlikely]] {
        ZHLN::Log(
            "[DescriptorHeap] {} heap write out of range: slot {} >= capacity {}. Dropping {} descriptor write(s).",
            HeapTypeName(_type), slot, _capacity, batchCount
        );
        return false;
    }
    if (!IsSlotWritable(slot)) [[unlikely]] {
        ZHLN::Log(
            "[DescriptorHeap] {} heap write targets unallocated or stale slot {}. Dropping {} descriptor write(s).",
            HeapTypeName(_type), slot, batchCount
        );
        return false;
    }
    return true;
}

void DescriptorHeap::FlushHostCache(VkDeviceSize offset, VkDeviceSize size) noexcept {
    if (_allocator != nullptr) {
        _buffer.Flush(*_allocator, offset, size);
    }
}

void DescriptorHeap::Write(const ResourceWriteBatch& batch) noexcept {
    WriteResources(batch.Entries());
}

void DescriptorHeap::Write(const SamplerWriteBatch& batch) noexcept {
    WriteSamplers(batch.Entries());
}

void DescriptorHeap::WriteResource(const ResourceWriteEntry& entry) noexcept {
    WriteResources(std::span<const ResourceWriteEntry> {&entry, 1});
}

void DescriptorHeap::WriteSampler(SamplerHandle handle, Sampler sampler) noexcept {
    if (!handle.Valid()) {
        return;
    }
    const SamplerWriteEntry entry {.slot = handle.index, .sampler = sampler};
    WriteSamplers(std::span<const SamplerWriteEntry> {&entry, 1});
}

void DescriptorHeap::WriteResources(std::span<const ResourceWriteEntry> entries) noexcept {
    if (_type != DescriptorHeapType::Resources || !Valid() || vkWriteResourceDescriptorsEXT == nullptr || _stride == 0 || entries.empty()) {
        return;
    }

    const auto totalCount = static_cast<uint32_t>(entries.size());
    uint32_t   minSlot    = entries.front().slot;
    uint32_t   maxSlot    = minSlot;
    for (const auto& entry: entries) {
        if (!ValidateWriteSlot(entry.slot, totalCount)) [[unlikely]] {
            return;
        }
        minSlot = std::min(minSlot, entry.slot);
        maxSlot = std::max(maxSlot, entry.slot);
    }

    std::array<VkImageViewCreateInfo, kDescriptorWriteChunk>       viewInfos {};
    std::array<VkImageDescriptorInfoEXT, kDescriptorWriteChunk>    imageInfos {};
    std::array<VkDeviceAddressRangeEXT, kDescriptorWriteChunk>     addressRanges {};
    std::array<VkResourceDescriptorInfoEXT, kDescriptorWriteChunk> descriptors {};
    std::array<VkHostAddressRangeEXT, kDescriptorWriteChunk>       hostRanges {};
    auto* const                                                    base = static_cast<uint8_t*>(_mappedPtr);

    for (uint32_t first = 0; first < totalCount; first += kDescriptorWriteChunk) {
        const uint32_t count = std::min(kDescriptorWriteChunk, totalCount - first);
        for (uint32_t i = 0; i < count; ++i) {
            const auto& entry = entries[first + i];
            descriptors[i]    = {
                .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
                .pNext = nullptr,
                .type  = entry.type,
                .data  = {},
            };
            if (entry.kind == ResourceWriteEntry::Kind::Image) {
                viewInfos[i]  = entry.payload.view.AsCreateInfo();
                imageInfos[i] = {
                    .sType  = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
                    .pNext  = nullptr,
                    .pView  = &viewInfos[i],
                    .layout = entry.layout,
                };
                descriptors[i].data.pImage = &imageInfos[i];
            } else {
                addressRanges[i]                  = entry.payload.range;
                descriptors[i].data.pAddressRange = &addressRanges[i];
            }
            hostRanges[i] = {
                .address = base + (static_cast<VkDeviceSize>(entry.slot) * _stride),
                .size    = _stride,
            };
        }
        vkWriteResourceDescriptorsEXT(_device, count, descriptors.data(), hostRanges.data());
    }

    VkDeviceSize flushOffset = static_cast<VkDeviceSize>(minSlot) * _stride;
    VkDeviceSize flushSize   = (static_cast<VkDeviceSize>(maxSlot) + 1U) * _stride - flushOffset;
    flushOffset              = ZHLN::Math::AlignDown(flushOffset, _nonCoherentAtomSize);
    flushSize                = ZHLN::Math::AlignUp(flushSize, _nonCoherentAtomSize);
    if (flushSize > 0) {
        FlushHostCache(flushOffset, flushSize);
    }
}

void DescriptorHeap::WriteSamplers(std::span<const SamplerWriteEntry> entries) noexcept {
    if (_type != DescriptorHeapType::Samplers || !Valid() || vkWriteSamplerDescriptorsEXT == nullptr || _stride == 0 || entries.empty()) {
        return;
    }

    const auto totalCount = static_cast<uint32_t>(entries.size());
    uint32_t   minSlot    = entries.front().slot;
    uint32_t   maxSlot    = minSlot;
    for (const auto& entry: entries) {
        if (!ValidateWriteSlot(entry.slot, totalCount)) [[unlikely]] {
            return;
        }
        minSlot = std::min(minSlot, entry.slot);
        maxSlot = std::max(maxSlot, entry.slot);
    }

    std::array<VkSamplerCreateInfo, kDescriptorWriteChunk>   createInfos {};
    std::array<VkHostAddressRangeEXT, kDescriptorWriteChunk> hostRanges {};
    auto* const                                              base = static_cast<uint8_t*>(_mappedPtr);

    for (uint32_t first = 0; first < totalCount; first += kDescriptorWriteChunk) {
        const uint32_t count = std::min(kDescriptorWriteChunk, totalCount - first);
        for (uint32_t i = 0; i < count; ++i) {
            const auto& entry = entries[first + i];
            createInfos[i]    = entry.sampler.AsCreateInfo();
            hostRanges[i]     = {
                .address = base + (static_cast<VkDeviceSize>(entry.slot) * _stride),
                .size    = _stride,
            };
        }
        vkWriteSamplerDescriptorsEXT(_device, count, createInfos.data(), hostRanges.data());
    }

    VkDeviceSize flushOffset = static_cast<VkDeviceSize>(minSlot) * _stride;
    VkDeviceSize flushSize   = (static_cast<VkDeviceSize>(maxSlot) + 1U) * _stride - flushOffset;
    flushOffset              = ZHLN::Math::AlignDown(flushOffset, _nonCoherentAtomSize);
    flushSize                = ZHLN::Math::AlignUp(flushSize, _nonCoherentAtomSize);
    if (flushSize > 0) {
        FlushHostCache(flushOffset, flushSize);
    }
}

auto DescriptorHeap::ResolveMapping(const DescriptorMapping& mapping, const DescriptorHeap* pairedSamplerHeap) const noexcept
    -> VkDescriptorSetAndBindingMappingEXT {
    const auto stride     = static_cast<uint32_t>(_stride);
    const auto byteOffset = static_cast<uint32_t>(static_cast<VkDeviceSize>(mapping.slot) * _stride);

    VkDescriptorSetAndBindingMappingEXT vkMapping {};
    vkMapping.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
    vkMapping.descriptorSet = mapping.descriptorSet;
    vkMapping.firstBinding  = mapping.firstBinding;
    vkMapping.bindingCount  = mapping.bindingCount;
    vkMapping.resourceMask  = mapping.resourceMask;

    switch (mapping.source) {
        case MappingSource::SamplerSlot:
        case MappingSource::ResourceSlot:
            vkMapping.source                                    = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
            vkMapping.sourceData.constantOffset.heapOffset      = byteOffset;
            vkMapping.sourceData.constantOffset.heapArrayStride = mapping.array ? stride : 0;
            break;
        case MappingSource::PushIndex: {
            const uint32_t samplerOffset = (mapping.pairedSampler && pairedSamplerHeap != nullptr) ?
                                               static_cast<uint32_t>(static_cast<VkDeviceSize>(mapping.samplerSlot) * pairedSamplerHeap->_stride) :
                                               0U;
            vkMapping.source                                 = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT;
            vkMapping.sourceData.pushIndex.heapOffset        = byteOffset;
            vkMapping.sourceData.pushIndex.pushOffset        = mapping.pushOffset;
            vkMapping.sourceData.pushIndex.heapIndexStride   = stride;
            vkMapping.sourceData.pushIndex.heapArrayStride   = mapping.array ? stride : 0;
            vkMapping.sourceData.pushIndex.samplerHeapOffset = samplerOffset;
            break;
        }
        case MappingSource::PushAddress:
            vkMapping.source                       = VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT;
            vkMapping.sourceData.pushAddressOffset = mapping.pushOffset;
            break;
    }

    return vkMapping;
}

// =============================================================================
// HeapManager & Transaction Scopes
// =============================================================================

auto HeapManager::Init(
    const Context& ctx,
    Allocator&     allocator,
    uint32_t       staticResourceCount,
    uint32_t       staticSamplerCount,
    uint32_t       frameTransientResourceCount,
    uint32_t       immediateTransientResourceCount,
    HeapConfig     config
) noexcept -> std::expected<void, ErrorCode> {
    _device = ctx.Device();

    auto resInit = _resourceHeap.Init(
        ctx, allocator, DescriptorHeapType::Resources,
        {
            .staticSlots             = staticResourceCount,
            .frameTransientSlots     = frameTransientResourceCount,
            .immediateTransientSlots = immediateTransientResourceCount,
        },
        config
    );
    if (!resInit.has_value()) [[unlikely]] {
        return std::unexpected(resInit.error());
    }

    auto sampInit = _samplerHeap.Init(
        ctx, allocator, DescriptorHeapType::Samplers,
        {
            .staticSlots = staticSamplerCount,
        },
        config
    );
    if (!sampInit.has_value()) [[unlikely]] {
        return std::unexpected(sampInit.error());
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

auto HeapManager::BeginFrameScope(uint32_t frameIndex) noexcept -> HeapFrameScope {
    EnterFrame(frameIndex);
    return HeapFrameScope(*this, frameIndex);
}

auto HeapManager::CreateImmediateScope() noexcept -> HeapImmediateScope {
    return HeapImmediateScope(*this);
}

void HeapManager::EnterFrame(uint32_t frameIndex) noexcept {
    const ZHLN::MutexGuard guard(_writeMutex);
    _resourceHeap.BeginFrame(frameIndex);
}

void HeapManager::LeaveFrame() noexcept {
    // Nothing to undo: the next frame's transaction resets the partition, and the
    // scope only exists so the frame's owner cannot forget that it was opened.
}

auto HeapManager::EnterImmediateScope() noexcept -> DescriptorHeap::ImmediateScope {
    const ZHLN::MutexGuard guard(_writeMutex);
    return _resourceHeap.CreateImmediateScope();
}

void HeapManager::LeaveImmediateScope(DescriptorHeap::ImmediateScope&& scope) noexcept {
    const ZHLN::MutexGuard guard(_writeMutex);
    scope.Reset();
}

HeapImmediateScope::HeapImmediateScope(HeapManager& owner) noexcept: _owner(&owner), _scope(owner.EnterImmediateScope()) {
}

HeapImmediateScope::~HeapImmediateScope() noexcept {
    Close();
}

HeapImmediateScope::HeapImmediateScope(HeapImmediateScope&& other) noexcept:
    _owner(std::exchange(other._owner, nullptr)), _scope(std::move(other._scope)) {
}

auto HeapImmediateScope::operator=(HeapImmediateScope&& other) noexcept -> HeapImmediateScope& {
    if (this != &other) {
        Close();
        _owner = std::exchange(other._owner, nullptr);
        _scope = std::move(other._scope);
    }
    return *this;
}

void HeapImmediateScope::Close() noexcept {
    if (_owner != nullptr) {
        _owner->LeaveImmediateScope(std::move(_scope));
        _owner = nullptr;
    }
}

HeapFrameScope::HeapFrameScope(HeapManager& owner, uint32_t frameIndex) noexcept: _owner(&owner), _frameIndex(frameIndex) {
}

HeapFrameScope::~HeapFrameScope() noexcept {
    Close();
}

HeapFrameScope::HeapFrameScope(HeapFrameScope&& other) noexcept:
    _owner(std::exchange(other._owner, nullptr)), _frameIndex(std::exchange(other._frameIndex, 0)) {
}

auto HeapFrameScope::operator=(HeapFrameScope&& other) noexcept -> HeapFrameScope& {
    if (this != &other) {
        Close();
        _owner      = std::exchange(other._owner, nullptr);
        _frameIndex = std::exchange(other._frameIndex, 0);
    }
    return *this;
}

void HeapFrameScope::Close() noexcept {
    if (_owner != nullptr) {
        _owner->LeaveFrame();
        _owner = nullptr;
    }
}

auto HeapManager::AllocateStaticResourceSlot() noexcept -> std::expected<uint32_t, ErrorCode> {
    const ZHLN::MutexGuard guard(_writeMutex);
    return _resourceHeap.AllocateStatic();
}

void HeapManager::FreeStaticResourceSlot(uint32_t slot) noexcept {
    const ZHLN::MutexGuard guard(_writeMutex);
    _resourceHeap.FreeStatic(slot);
}

auto HeapManager::AllocateStaticSamplerSlot() noexcept -> std::expected<uint32_t, ErrorCode> {
    const ZHLN::MutexGuard guard(_writeMutex);
    return _samplerHeap.AllocateStatic();
}

void HeapManager::FreeStaticSamplerSlot(uint32_t slot) noexcept {
    const ZHLN::MutexGuard guard(_writeMutex);
    _samplerHeap.FreeStatic(slot);
}

auto HeapManager::AllocateTransientResourceRange(uint32_t count, HeapLifecycle lifecycle) noexcept -> std::expected<uint32_t, ErrorCode> {
    const ZHLN::MutexGuard guard(_writeMutex);
    return _resourceHeap.AllocateTransientRange(count, lifecycle);
}

auto HeapManager::ReserveSampler() noexcept -> std::expected<SamplerHandle, ErrorCode> {
    return AllocateStaticSamplerSlot().transform([](uint32_t slot) { return SamplerHandle {slot}; });
}

auto HeapManager::ReserveImage() noexcept -> std::expected<TextureHandle, ErrorCode> {
    return AllocateStaticResourceSlot().transform([](uint32_t slot) { return TextureHandle {slot}; });
}

auto HeapManager::ReserveSamplerRange(uint32_t count) noexcept -> std::expected<SamplerHandle, ErrorCode> {
    const ZHLN::MutexGuard guard(_writeMutex);
    return _samplerHeap.AllocateStaticRange(count).transform([](uint32_t base) { return SamplerHandle {base}; });
}

auto HeapManager::ReserveImageRange(uint32_t count) noexcept -> std::expected<TextureHandle, ErrorCode> {
    const ZHLN::MutexGuard guard(_writeMutex);
    return _resourceHeap.AllocateStaticRange(count).transform([](uint32_t base) { return TextureHandle {base}; });
}

auto HeapManager::RegisterSampler(Sampler sampler) noexcept -> std::expected<SamplerHandle, ErrorCode> {
    auto handle = ReserveSampler();
    if (!handle) [[unlikely]] {
        return handle;
    }
    BindSampler(*handle, sampler);
    return handle;
}

auto HeapManager::RegisterImage(const ImageView& view, VkImageLayout layout) noexcept -> std::expected<TextureHandle, ErrorCode> {
    auto handle = ReserveImage();
    if (!handle) [[unlikely]] {
        return handle;
    }
    BindImage(*handle, view, layout);
    return handle;
}

auto HeapManager::RegisterStorageImage(const ImageView& view, VkImageLayout layout) noexcept -> std::expected<StorageImageHandle, ErrorCode> {
    auto slot = AllocateStaticResourceSlot();
    if (!slot) [[unlikely]] {
        return std::unexpected(slot.error());
    }
    BindStorageImage(StorageImageHandle {*slot}, view, layout);
    return StorageImageHandle {*slot};
}

auto HeapManager::RegisterStorageBuffer(BufferSlice slice) noexcept -> std::expected<StorageBufferHandle, ErrorCode> {
    auto slot = AllocateStaticResourceSlot();
    if (!slot) [[unlikely]] {
        return std::unexpected(slot.error());
    }
    BindStorageBuffer(StorageBufferHandle {*slot}, slice);
    return StorageBufferHandle {*slot};
}

auto HeapManager::RegisterUniformBuffer(BufferSlice slice) noexcept -> std::expected<UniformBufferHandle, ErrorCode> {
    auto slot = AllocateStaticResourceSlot();
    if (!slot) [[unlikely]] {
        return std::unexpected(slot.error());
    }
    BindUniformBuffer(UniformBufferHandle {*slot}, slice);
    return UniformBufferHandle {*slot};
}

auto HeapManager::RegisterAccelerationStructure(VkDeviceAddress address) noexcept -> std::expected<AccelerationStructureHandle, ErrorCode> {
    auto slot = AllocateStaticResourceSlot();
    if (!slot) [[unlikely]] {
        return std::unexpected(slot.error());
    }
    BindAccelerationStructure(AccelerationStructureHandle {*slot}, address);
    return AccelerationStructureHandle {*slot};
}

void HeapManager::ReleaseSampler(SamplerHandle handle) noexcept {
    if (handle.Valid()) {
        FreeStaticSamplerSlot(handle.index);
    }
}

void HeapManager::WriteResources(const ResourceWriteBatch& batch) noexcept {
    if (batch.Empty()) {
        return;
    }
    const ZHLN::MutexGuard guard(_writeMutex);
    _resourceHeap.Write(batch);
}

void HeapManager::WriteSamplers(const SamplerWriteBatch& batch) noexcept {
    if (batch.Empty()) {
        return;
    }
    const ZHLN::MutexGuard guard(_writeMutex);
    _samplerHeap.Write(batch);
}

void HeapManager::BindHeaps(VkCommandBuffer cmd) const noexcept {
    _resourceHeap.Bind(cmd);
    _samplerHeap.Bind(cmd);
}

void HeapManager::ApplySamplerMapping(HeapMappingTable& mappings, uint32_t set, uint32_t binding, SamplerHandle handle) const noexcept {
    mappings.push_back(
        {.descriptorSet = set,
         .firstBinding  = binding,
         .resourceMask  = VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT,
         .source        = MappingSource::SamplerSlot,
         .slot          = handle.index}
    );
}

void HeapManager::ApplySamplerArrayMapping(HeapMappingTable& mappings, uint32_t set, uint32_t binding, SamplerHandle base) const noexcept {
    mappings.push_back(
        {.descriptorSet = set,
         .firstBinding  = binding,
         .resourceMask  = VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT,
         .source        = MappingSource::SamplerSlot,
         .slot          = base.index,
         .array         = true}
    );
}

void HeapManager::ApplySampledImageMapping(HeapMappingTable& mappings, uint32_t set, uint32_t binding, TextureHandle handle) const noexcept {
    mappings.push_back(
        {.descriptorSet = set,
         .firstBinding  = binding,
         .resourceMask  = VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT,
         .source        = MappingSource::ResourceSlot,
         .slot          = handle.index}
    );
}

void HeapManager::ApplyBindlessTextureArrayMapping(HeapMappingTable& mappings, uint32_t set, uint32_t binding, TextureHandle base) const noexcept {
    mappings.push_back(
        {.descriptorSet = set,
         .firstBinding  = binding,
         .resourceMask  = VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT,
         .source        = MappingSource::ResourceSlot,
         .slot          = base.index,
         .array         = true}
    );
}

void HeapManager::ApplyPushIndexMapping(
    HeapMappingTable&           mappings,
    uint32_t                    set,
    uint32_t                    binding,
    VkSpirvResourceTypeFlagsEXT mask,
    uint32_t                    ordinal,
    uint32_t                    pushOffset
) const noexcept {
    // The ordinal is a row index into the heap; the byte offset it implies is
    // resolved by DescriptorHeap::ResolveMapping() at Materialize() time.
    mappings.push_back(
        {.descriptorSet = set,
         .firstBinding  = binding,
         .resourceMask  = mask,
         .source        = MappingSource::PushIndex,
         .slot          = ordinal,
         .pushOffset    = pushOffset}
    );
}

auto HeapManager::ApplyNewSamplerMapping(HeapMappingTable& mappings, uint32_t set, uint32_t binding, VkSpirvResourceTypeFlagsEXT mask) noexcept
    -> std::expected<SamplerHandle, ErrorCode> {
    auto slot = AllocateStaticSamplerSlot();
    if (!slot) [[unlikely]] {
        return std::unexpected(slot.error());
    }
    mappings.push_back(
        {.descriptorSet = set, .firstBinding = binding, .resourceMask = mask, .source = MappingSource::SamplerSlot, .slot = *slot}
    );
    return SamplerHandle {*slot};
}

auto HeapManager::ApplyPairedSamplerMapping(DescriptorMapping& entry) noexcept -> std::expected<SamplerHandle, ErrorCode> {
    auto slot = AllocateStaticSamplerSlot();
    if (!slot) [[unlikely]] {
        return std::unexpected(slot.error());
    }
    entry.pairedSampler = true;
    entry.samplerSlot   = *slot;
    return SamplerHandle {*slot};
}

void HeapManager::Materialize(HeapMappingView mappings, std::span<VkDescriptorSetAndBindingMappingEXT> out) const noexcept {
    const auto count = std::min(mappings.size(), out.size());
    for (size_t i = 0; i < count; ++i) {
        const DescriptorMapping& m = mappings[i];
        out[i] = (m.source == MappingSource::SamplerSlot) ? _samplerHeap.ResolveMapping(m) :
                                                            _resourceHeap.ResolveMapping(m, &_samplerHeap);
    }
}

void HeapManager::BindImage(TextureHandle handle, const ImageView& view, VkImageLayout layout) noexcept {
    if (!handle.Valid() || view.Handle() == VK_NULL_HANDLE) {
        return;
    }
    const auto               entry = ResourceWriteEntry::Image(handle.index, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, view, layout);
    const ZHLN::MutexGuard   guard(_writeMutex);
    _resourceHeap.WriteResource(entry);
}

void HeapManager::BindStorageImage(StorageImageHandle handle, const ImageView& view, VkImageLayout layout) noexcept {
    if (!handle.Valid() || view.Handle() == VK_NULL_HANDLE) {
        return;
    }
    const auto               entry = ResourceWriteEntry::Image(handle.index, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, view, layout);
    const ZHLN::MutexGuard   guard(_writeMutex);
    _resourceHeap.WriteResource(entry);
}

void HeapManager::BindStorageBuffer(StorageBufferHandle handle, BufferSlice slice) noexcept {
    if (!handle.Valid()) {
        return;
    }
    const auto entry = ResourceWriteEntry::Address(
        handle.index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, {.address = slice.Address().Get(), .size = slice.Size()}
    );
    const ZHLN::MutexGuard guard(_writeMutex);
    _resourceHeap.WriteResource(entry);
}

void HeapManager::BindUniformBuffer(UniformBufferHandle handle, BufferSlice slice) noexcept {
    if (!handle.Valid()) {
        return;
    }
    const auto entry = ResourceWriteEntry::Address(
        handle.index, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, {.address = slice.Address().Get(), .size = slice.Size()}
    );
    const ZHLN::MutexGuard guard(_writeMutex);
    _resourceHeap.WriteResource(entry);
}

void HeapManager::BindAccelerationStructure(AccelerationStructureHandle handle, VkDeviceAddress address) noexcept {
    if (!handle.Valid()) {
        return;
    }
    const auto             entry = ResourceWriteEntry::Address(handle.index, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, {.address = address, .size = 0});
    const ZHLN::MutexGuard guard(_writeMutex);
    _resourceHeap.WriteResource(entry);
}

void HeapManager::BindSampler(SamplerHandle handle, Sampler sampler) noexcept {
    if (!handle.Valid()) {
        return;
    }
    const ZHLN::MutexGuard guard(_writeMutex);
    _samplerHeap.WriteSampler(handle, sampler);
}

} // namespace ZHLN::Vk
