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
    if constexpr (Type == DescriptorHeapType::Samplers) {
        Debug::SetBufferName(ctx, _buffer.Handle(), "DescriptorHeap.Samplers");
    } else {
        Debug::SetBufferName(ctx, _buffer.Handle(), "DescriptorHeap.Resources");
    }

    auto mapped = _buffer.Map(*_allocator);
    if (!mapped) [[unlikely]] {
        return std::unexpected(mapped.error());
    }
    _mappedRegion = std::move(*mapped);
    _mappedPtr    = _mappedRegion.Data();

    const VkDeviceAddress address = ctx.BufferAddress(_buffer.Handle());
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
    struct ImagePayload {
        ImageView                view {};
        // The heap extension consumes the create info through a pointer at flush time, so the
        // synthesized struct has to outlive the batch. Everything else stays an ImageView.
        VkImageViewCreateInfo    info {};
        VkImageDescriptorInfoEXT descriptor {};

        ImagePayload(const ImageView& imageView, VkImageLayout layout) noexcept:
            view(imageView),
            info(imageView.AsCreateInfo()),
            descriptor {.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT, .pNext = nullptr, .pView = &info, .layout = layout} {
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

    void AddImage(uint32_t slot, VkDescriptorType type, const ImageView& view, VkImageLayout layout) {
        std::pmr::polymorphic_allocator<ImagePayload> alloc {&arena};
        auto*                                         image = alloc.new_object<ImagePayload>(view, layout);

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

void ResourceWriteBatch::AddImage(TextureHandle handle, const ImageView& view, VkImageLayout layout) noexcept {
    if (handle.Valid() && view.Handle() != VK_NULL_HANDLE) {
        _impl->AddImage(handle.index, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, view, layout);
    }
}

void ResourceWriteBatch::AddStorageImage(StorageImageHandle handle, const ImageView& view, VkImageLayout layout) noexcept {
    if (handle.Valid() && view.Handle() != VK_NULL_HANDLE) {
        _impl->AddImage(handle.index, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, view, layout);
    }
}

void ResourceWriteBatch::AddBuffer(StorageBufferHandle handle, BufferSlice slice) noexcept {
    _impl->AddAddress(handle.index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, {.address = slice.Address().Get(), .size = slice.Size()});
}

void ResourceWriteBatch::AddBuffer(UniformBufferHandle handle, BufferSlice slice) noexcept {
    _impl->AddAddress(handle.index, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, {.address = slice.Address().Get(), .size = slice.Size()});
}

void ResourceWriteBatch::AddAccelerationStructure(AccelerationStructureHandle handle, VkDeviceAddress address) noexcept {
    _impl->AddAddress(handle.index, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, {.address = address, .size = 0});
}

void ResourceWriteBatch::Flush(VkDevice device, void* mappedPtr, VkDeviceSize stride) noexcept {
    const auto total_count = static_cast<uint32_t>(_impl->writes.size());
    if (total_count == 0) {
        return;
    }

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

auto HeapManager::BeginFrameScope(uint32_t frameIndex) noexcept -> HeapFrameScope {
    EnterFrame(frameIndex);
    return HeapFrameScope(*this, frameIndex);
}

auto HeapManager::CreateImmediateScope() noexcept -> HeapImmediateScope {
    return HeapImmediateScope(*this);
}

void HeapManager::EnterFrame(uint32_t frameIndex) noexcept {
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

void HeapManager::LeaveFrame() noexcept {
    // Nothing to undo: the next frame's transaction resets the partition, and the
    // scope only exists so the frame's owner cannot forget that it was opened.
}

auto HeapManager::EnterImmediateScope() noexcept -> uint32_t {
    const ZHLN::MutexGuard guard(_writeMutex);
    return _immediateTransientAllocated;
}

void HeapManager::LeaveImmediateScope(uint32_t baseCursor) noexcept {
    const ZHLN::MutexGuard guard(_writeMutex);
    ZHLN::Assert(baseCursor <= _immediateTransientAllocated, "descriptor-heap immediate scopes must nest LIFO");
    _immediateTransientAllocated = baseCursor;
}

HeapImmediateScope::HeapImmediateScope(HeapManager& owner) noexcept: _owner(&owner), _baseCursor(owner.EnterImmediateScope()) {
}

HeapImmediateScope::~HeapImmediateScope() noexcept {
    Close();
}

HeapImmediateScope::HeapImmediateScope(HeapImmediateScope&& other) noexcept:
    _owner(std::exchange(other._owner, nullptr)), _baseCursor(std::exchange(other._baseCursor, 0)) {
}

auto HeapImmediateScope::operator=(HeapImmediateScope&& other) noexcept -> HeapImmediateScope& {
    if (this != &other) {
        Close();
        _owner      = std::exchange(other._owner, nullptr);
        _baseCursor = std::exchange(other._baseCursor, 0);
    }
    return *this;
}

void HeapImmediateScope::Close() noexcept {
    if (_owner != nullptr) {
        _owner->LeaveImmediateScope(_baseCursor);
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

auto HeapManager::ReserveSampler() noexcept -> std::expected<SamplerHandle, ErrorCode> {
    return AllocateStaticSamplerSlot().transform([](uint32_t slot) { return SamplerHandle {slot}; });
}

auto HeapManager::ReserveImage() noexcept -> std::expected<TextureHandle, ErrorCode> {
    return AllocateStaticResourceSlot().transform([](uint32_t slot) { return TextureHandle {slot}; });
}

auto HeapManager::ReserveSamplerRange(uint32_t count) noexcept -> std::expected<SamplerHandle, ErrorCode> {
    const uint32_t base = _staticSamplerAlloc.Cursor();
    if (count > _staticSamplerCount - base) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::SamplerSlotsExhausted);
    }
    _staticSamplerAlloc.Skip(count);
    return SamplerHandle {base};
}

auto HeapManager::ReserveImageRange(uint32_t count) noexcept -> std::expected<TextureHandle, ErrorCode> {
    const uint32_t base = _staticResourceAlloc.Cursor();
    if (count > _staticResourceCount - base) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::ResourceSlotsExhausted);
    }
    _staticResourceAlloc.Skip(count);
    return TextureHandle {base};
}

auto HeapManager::RegisterSampler(const SamplerConfig& config) noexcept -> std::expected<SamplerHandle, ErrorCode> {
    auto handle = ReserveSampler();
    if (!handle) [[unlikely]] {
        return handle;
    }
    BindSampler(*handle, config);
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
    // computed at Materialize() time, where the stride lives.
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
    const auto resourceStride = static_cast<uint32_t>(ResourceStride());
    const auto samplerStride  = static_cast<uint32_t>(SamplerStride());
    const auto count          = std::min(mappings.size(), out.size());

    for (size_t i = 0; i < count; ++i) {
        const DescriptorMapping&              m = mappings[i];
        VkDescriptorSetAndBindingMappingEXT& e = out[i];

        e               = {};
        e.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
        e.descriptorSet = m.descriptorSet;
        e.firstBinding  = m.firstBinding;
        e.bindingCount  = m.bindingCount;
        e.resourceMask  = m.resourceMask;

        switch (m.source) {
            case MappingSource::SamplerSlot:
                e.source                                    = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
                e.sourceData.constantOffset.heapOffset      = static_cast<uint32_t>(SamplerOffset(m.slot));
                e.sourceData.constantOffset.heapArrayStride = m.array ? samplerStride : 0;
                break;
            case MappingSource::ResourceSlot:
                e.source                                    = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
                e.sourceData.constantOffset.heapOffset      = static_cast<uint32_t>(ResourceOffset(m.slot));
                e.sourceData.constantOffset.heapArrayStride = m.array ? resourceStride : 0;
                break;
            case MappingSource::PushIndex:
                e.source                                 = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT;
                e.sourceData.pushIndex.heapOffset        = m.slot * resourceStride;
                e.sourceData.pushIndex.pushOffset        = m.pushOffset;
                e.sourceData.pushIndex.heapIndexStride   = resourceStride;
                e.sourceData.pushIndex.heapArrayStride   = m.array ? resourceStride : 0;
                e.sourceData.pushIndex.samplerHeapOffset = m.pairedSampler ? static_cast<uint32_t>(SamplerOffset(m.samplerSlot)) : 0;
                break;
            case MappingSource::PushAddress:
                e.source                       = VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT;
                e.sourceData.pushAddressOffset = m.pushOffset;
                break;
        }
    }
}

void HeapManager::BindImage(TextureHandle handle, const ImageView& view, VkImageLayout layout) noexcept {
    if (!handle.Valid() || view.Handle() == VK_NULL_HANDLE) {
        return;
    }
    ResourceWriteBatch batch;
    batch.AddImage(handle, view, layout);
    FlushResourceBatch(batch);
}

void HeapManager::BindStorageImage(StorageImageHandle handle, const ImageView& view, VkImageLayout layout) noexcept {
    if (!handle.Valid() || view.Handle() == VK_NULL_HANDLE) {
        return;
    }
    ResourceWriteBatch batch;
    batch.AddStorageImage(handle, view, layout);
    FlushResourceBatch(batch);
}

void HeapManager::BindStorageBuffer(StorageBufferHandle handle, BufferSlice slice) noexcept {
    if (!handle.Valid()) {
        return;
    }
    ResourceWriteBatch batch;
    batch.AddBuffer(handle, slice);
    FlushResourceBatch(batch);
}

void HeapManager::BindUniformBuffer(UniformBufferHandle handle, BufferSlice slice) noexcept {
    if (!handle.Valid()) {
        return;
    }
    ResourceWriteBatch batch;
    batch.AddBuffer(handle, slice);
    FlushResourceBatch(batch);
}

void HeapManager::BindAccelerationStructure(AccelerationStructureHandle handle, VkDeviceAddress address) noexcept {
    if (!handle.Valid()) {
        return;
    }
    ResourceWriteBatch batch;
    batch.AddAccelerationStructure(handle, address);
    FlushResourceBatch(batch);
}

void HeapManager::BindSampler(SamplerHandle handle, const SamplerConfig& config) noexcept {
    if (!handle.Valid()) {
        return;
    }
    SamplerWriteBatch batch;
    batch.AddSampler(handle, config);
    FlushSamplerBatch(batch);
}

template class DescriptorHeap<DescriptorHeapType::Resources>;
template class DescriptorHeap<DescriptorHeapType::Samplers>;

} // namespace ZHLN::Vk
