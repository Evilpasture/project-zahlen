// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "memory/Allocator.hpp"
#include "DescriptorMapping.hpp"

#include <Zahlen/Threading/Mutex.hpp>
#include <utility>
#include <vector>

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

class ResourceWriteBatch;
class SamplerWriteBatch;
class HeapManager;
class HeapImmediateScope;
class HeapFrameScope;
class Sampler;
struct HeapPassBindings;

enum class DescriptorHeapType : uint8_t {
    Resources,
    Samplers
};

enum class HeapLifecycle : uint8_t {
    Frame,
    Immediate
};

enum class DescriptorHeapError : uint8_t {
    ExtensionUnavailable = 1,
    ResourceSlotsExhausted,
    SamplerSlotsExhausted,
    TransientResourceOverflow,
    FunctionLoaderFailed,
    AllocationFailed,
    MappingFailed,
    DeviceAddressFailed,
    HeapTooLarge
};

template <DescriptorHeapType Heap, VkDescriptorType Type>
struct HeapHandle {
    uint32_t index = kInvalidIndex;

    static constexpr uint32_t kInvalidIndex = ~0U;

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return index != kInvalidIndex;
    }
    explicit constexpr operator bool() const noexcept {
        return Valid();
    }

    constexpr bool operator==(const HeapHandle&) const noexcept = default;
};

template <DescriptorHeapType Heap, VkDescriptorType Type>
inline constexpr HeapHandle<Heap, Type> kInvalidHandle {HeapHandle<Heap, Type>::kInvalidIndex};

using TextureHandle               = HeapHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>;
using StorageImageHandle          = HeapHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE>;
using UniformBufferHandle         = HeapHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER>;
using StorageBufferHandle         = HeapHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER>;
using SamplerHandle               = HeapHandle<DescriptorHeapType::Samplers, VK_DESCRIPTOR_TYPE_SAMPLER>;
using AccelerationStructureHandle = HeapHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR>;

template <VkDescriptorType Type>
concept ValidResourceDescriptorType = Type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || Type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                                      Type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || Type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                                      Type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

inline constexpr TextureHandle               kInvalidTextureHandle       = kInvalidHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>;
inline constexpr StorageImageHandle          kInvalidStorageImageHandle  = kInvalidHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE>;
inline constexpr UniformBufferHandle         kInvalidUniformBufferHandle = kInvalidHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER>;
inline constexpr StorageBufferHandle         kInvalidStorageBufferHandle = kInvalidHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER>;
inline constexpr SamplerHandle               kInvalidSamplerHandle       = kInvalidHandle<DescriptorHeapType::Samplers, VK_DESCRIPTOR_TYPE_SAMPLER>;
inline constexpr AccelerationStructureHandle kInvalidAccelerationStructureHandle =
    kInvalidHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR>;

static_assert(sizeof(TextureHandle) == sizeof(uint32_t));
static_assert(std::is_standard_layout_v<TextureHandle>);
static_assert(std::is_trivially_copyable_v<TextureHandle>);

template <DescriptorHeapType Type>
class DescriptorHeap {
  public:
    DescriptorHeap() = default;
    ~DescriptorHeap() noexcept;

    DescriptorHeap(const DescriptorHeap&)                    = delete;
    auto operator=(const DescriptorHeap&) -> DescriptorHeap& = delete;

    DescriptorHeap(DescriptorHeap&& other) noexcept;
    auto operator=(DescriptorHeap&& other) noexcept -> DescriptorHeap&;

    [[nodiscard]] auto Init(const Context& ctx, Allocator& allocator, uint32_t capacity) noexcept -> std::expected<void, ErrorCode>;
    void               Cleanup() noexcept;

    void Bind(VkCommandBuffer cmd) const noexcept;

    [[nodiscard]] auto GetBindInfo() const noexcept -> VkBindHeapInfoEXT {
        return _bindInfo;
    }

    void Flush(ResourceWriteBatch& batch) noexcept
        requires(Type == DescriptorHeapType::Resources);
    void Flush(SamplerWriteBatch& batch) noexcept
        requires(Type == DescriptorHeapType::Samplers);

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _buffer.Valid();
    }
    explicit operator bool() const noexcept {
        return Valid();
    }

    [[nodiscard]] auto SlotOffset(uint32_t slot) const noexcept -> VkDeviceSize {
        return static_cast<VkDeviceSize>(slot) * _stride;
    }
    [[nodiscard]] auto GetStride() const noexcept -> VkDeviceSize {
        return _stride;
    }
    [[nodiscard]] auto GetCapacity() const noexcept -> uint32_t {
        return _capacity;
    }
    [[nodiscard]] auto GetReservedSize() const noexcept -> VkDeviceSize {
        return _reservedSize;
    }

  private:
    friend class HeapManager;

    [[nodiscard]] auto GetDevice() const noexcept -> VkDevice {
        return _device;
    }
    [[nodiscard]] auto GetMappedPtr() const noexcept -> void* {
        return _mappedPtr;
    }

    void FlushHostCache(VkDeviceSize offset, VkDeviceSize size) noexcept;

    VkDevice     _device       = VK_NULL_HANDLE;
    uint32_t     _capacity     = 0;
    VkDeviceSize _stride       = 0;
    VkDeviceSize _reservedSize = 0;
    VkDeviceSize _nonCoherentAtomSize = 1;

    Allocator*           _allocator = nullptr;
    Buffer               _buffer;
    Buffer::MappedRegion _mappedRegion;
    void*                _mappedPtr = nullptr;
    VkBindHeapInfoEXT    _bindInfo  = {};
};

class ResourceWriteBatch {
  public:
    ResourceWriteBatch() noexcept;
    ~ResourceWriteBatch() noexcept;

    ResourceWriteBatch(const ResourceWriteBatch&)                    = delete;
    auto operator=(const ResourceWriteBatch&) -> ResourceWriteBatch& = delete;

    ResourceWriteBatch(ResourceWriteBatch&& other) noexcept;
    auto operator=(ResourceWriteBatch&& other) noexcept -> ResourceWriteBatch&;

    void AddImage(TextureHandle handle, const ImageView& view, VkImageLayout layout) noexcept;
    void AddStorageImage(StorageImageHandle handle, const ImageView& view, VkImageLayout layout) noexcept;
    void AddBuffer(StorageBufferHandle handle, BufferSlice slice) noexcept;
    void AddBuffer(UniformBufferHandle handle, BufferSlice slice) noexcept;
    void AddAccelerationStructure(AccelerationStructureHandle handle, VkDeviceAddress address) noexcept;

    void Flush(VkDevice device, void* mappedPtr, VkDeviceSize stride) noexcept;

    [[nodiscard]] auto Empty() const noexcept -> bool;
    [[nodiscard]] auto SlotCount() const noexcept -> uint32_t;

    [[nodiscard]] auto SlotBounds() const noexcept -> std::pair<uint32_t, uint32_t>;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

class SamplerWriteBatch {
  public:
    SamplerWriteBatch() noexcept;
    ~SamplerWriteBatch() noexcept;

    SamplerWriteBatch(const SamplerWriteBatch&)                    = delete;
    auto operator=(const SamplerWriteBatch&) -> SamplerWriteBatch& = delete;

    SamplerWriteBatch(SamplerWriteBatch&& other) noexcept;
    auto operator=(SamplerWriteBatch&& other) noexcept -> SamplerWriteBatch&;

    void AddSampler(SamplerHandle handle, const Sampler& sampler) noexcept;

    void Flush(VkDevice device, void* mappedPtr, VkDeviceSize stride) noexcept;

    [[nodiscard]] auto Empty() const noexcept -> bool;
    [[nodiscard]] auto SlotCount() const noexcept -> uint32_t;
    [[nodiscard]] auto SlotsData() const noexcept -> const uint32_t*;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

class SlotAllocator {
  public:
    SlotAllocator() noexcept;
    ~SlotAllocator() noexcept;

    SlotAllocator(const SlotAllocator&)                    = delete;
    auto operator=(const SlotAllocator&) -> SlotAllocator& = delete;

    SlotAllocator(SlotAllocator&& other) noexcept;
    auto operator=(SlotAllocator&& other) noexcept -> SlotAllocator&;

    void               Init(uint32_t capacity, ErrorCode errorOnExhaustion) noexcept;
    [[nodiscard]] auto Allocate() noexcept -> std::expected<uint32_t, ErrorCode>;
    void               Free(uint32_t slot) noexcept;
    void               Skip(uint32_t count) noexcept;
    [[nodiscard]] auto Cursor() const noexcept -> uint32_t;
    void               Clear() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

class HeapManager {
  public:
    HeapManager()           = default;
    ~HeapManager() noexcept = default;

    HeapManager(const HeapManager&)                    = delete;
    auto operator=(const HeapManager&) -> HeapManager& = delete;

    HeapManager(HeapManager&&) noexcept                    = default;
    auto operator=(HeapManager&&) noexcept -> HeapManager& = default;

    [[nodiscard]] auto Init(
        const Context& ctx,
        Allocator&     allocator,
        uint32_t       staticResourceCount,
        uint32_t       staticSamplerCount,
        uint32_t       frameTransientResourceCount,
        uint32_t       immediateTransientResourceCount
    ) noexcept -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _resourceHeap.Valid() && _samplerHeap.Valid();
    }

    // -------------------------------------------------------------------------
    // Frame and immediate transactions
    // -------------------------------------------------------------------------
    /// Opens one frame's transient partition. The frame owner holds the token for
    /// the frame's duration; passes never tick the cursor by hand and never see the
    /// reset, so an early-returning frame cannot leave it stale.
    [[nodiscard]] auto BeginFrameScope(uint32_t frameIndex) noexcept -> HeapFrameScope;

    /// Opens a bump transaction over the immediate partition. The cursor is
    /// restored to the value it had on construction, so two live scopes hand out
    /// disjoint slots instead of silently overwriting each other's descriptors.
    [[nodiscard]] auto CreateImmediateScope() noexcept -> HeapImmediateScope;

    // -------------------------------------------------------------------------
    // Registration: slot assignment and descriptor write are one operation, so a
    // handle that exists is a handle that is already written.
    // -------------------------------------------------------------------------
    [[nodiscard]] auto RegisterSampler(const Sampler& sampler) noexcept -> std::expected<SamplerHandle, ErrorCode>;

    // `SamplerAt(i)` builds the i-th variant, so the caller describes the array
    // (the nine material address-mode combinations) instead of materializing one.
    template <typename SamplerAt>
    [[nodiscard]] auto RegisterSamplerArray(uint32_t count, SamplerAt&& samplerAt) noexcept -> std::expected<SamplerHandle, ErrorCode> {
        auto base = ReserveSamplerRange(count);
        if (!base) [[unlikely]] {
            return base;
        }
        for (uint32_t i = 0; i < count; ++i) {
            BindSampler(SamplerHandle {base->index + i}, samplerAt(i));
        }
        return base;
    }

    [[nodiscard]] auto RegisterImage(const ImageView& view, VkImageLayout layout) noexcept -> std::expected<TextureHandle, ErrorCode>;
    [[nodiscard]] auto RegisterStorageImage(const ImageView& view, VkImageLayout layout) noexcept -> std::expected<StorageImageHandle, ErrorCode>;
    [[nodiscard]] auto RegisterStorageBuffer(BufferSlice slice) noexcept -> std::expected<StorageBufferHandle, ErrorCode>;
    [[nodiscard]] auto RegisterUniformBuffer(BufferSlice slice) noexcept -> std::expected<UniformBufferHandle, ErrorCode>;
    [[nodiscard]] auto RegisterAccelerationStructure(VkDeviceAddress address) noexcept -> std::expected<AccelerationStructureHandle, ErrorCode>;

    // -------------------------------------------------------------------------
    // Reservation: only for handles whose payload cannot exist yet, because the
    // mapping that addresses them is baked into a pipeline first. The result is a
    // typed capability, never a slot index, and it is expected to be bound.
    // -------------------------------------------------------------------------
    [[nodiscard]] auto ReserveSampler() noexcept -> std::expected<SamplerHandle, ErrorCode>;
    [[nodiscard]] auto ReserveImage() noexcept -> std::expected<TextureHandle, ErrorCode>;
    [[nodiscard]] auto ReserveSamplerRange(uint32_t count) noexcept -> std::expected<SamplerHandle, ErrorCode>;
    [[nodiscard]] auto ReserveImageRange(uint32_t count) noexcept -> std::expected<TextureHandle, ErrorCode>;

    // -------------------------------------------------------------------------
    // Binding a payload to a handle, registered or reserved.
    // -------------------------------------------------------------------------
    void BindSampler(SamplerHandle handle, const Sampler& sampler) noexcept;
    void BindImage(TextureHandle handle, const ImageView& view, VkImageLayout layout) noexcept;
    void BindStorageImage(StorageImageHandle handle, const ImageView& view, VkImageLayout layout) noexcept;
    void BindStorageBuffer(StorageBufferHandle handle, BufferSlice slice) noexcept;
    void BindUniformBuffer(UniformBufferHandle handle, BufferSlice slice) noexcept;
    void BindAccelerationStructure(AccelerationStructureHandle handle, VkDeviceAddress address) noexcept;

    void ReleaseSampler(SamplerHandle handle) noexcept;
    template <VkDescriptorType Type>
        requires ValidResourceDescriptorType<Type>
    void ReleaseResource(HeapHandle<DescriptorHeapType::Resources, Type> handle) noexcept {
        FreeStaticResourceSlot(handle.index);
    }

    // -------------------------------------------------------------------------
    // Descriptor mapping. The heap owns its stride, padding and partition layout;
    // these append semantic entries naming slots, and Materialize() is the only
    // place that turns those slots into byte offsets. Nothing outside ever calls
    // ResourceStride()/SamplerOffset() and does the arithmetic itself.
    // -------------------------------------------------------------------------
    void ApplySamplerMapping(HeapMappingTable& mappings, uint32_t set, uint32_t binding, SamplerHandle handle) const noexcept;
    void ApplySamplerArrayMapping(HeapMappingTable& mappings, uint32_t set, uint32_t binding, SamplerHandle base) const noexcept;
    void ApplySampledImageMapping(HeapMappingTable& mappings, uint32_t set, uint32_t binding, TextureHandle handle) const noexcept;
    void ApplyBindlessTextureArrayMapping(HeapMappingTable& mappings, uint32_t set, uint32_t binding, TextureHandle base) const noexcept;
    void ApplyPushIndexMapping(
        HeapMappingTable&           mappings,
        uint32_t                    set,
        uint32_t                    binding,
        VkSpirvResourceTypeFlagsEXT mask,
        uint32_t                    ordinal,
        uint32_t                    pushOffset
    ) const noexcept;

    /// Allocates the sampler a reflected set binding addresses, and appends the
    /// mapping entry that points at it.
    [[nodiscard]] auto ApplyNewSamplerMapping(
        HeapMappingTable& mappings, uint32_t set, uint32_t binding, VkSpirvResourceTypeFlagsEXT mask
    ) noexcept -> std::expected<SamplerHandle, ErrorCode>;

    /// Pairs a freshly allocated sampler with an entry that already maps an image
    /// (COMBINED_IMAGE_SAMPLER).
    [[nodiscard]] auto ApplyPairedSamplerMapping(DescriptorMapping& entry) noexcept -> std::expected<SamplerHandle, ErrorCode>;

    /// The API boundary: writes `mappings` into `out` as the driver struct, with
    /// this heap's offsets and strides baked in. `out` must hold
    /// `mappings.size()` entries; the caller owns its lifetime, so the result
    /// never outlives the pipeline build that asked for it.
    void Materialize(HeapMappingView mappings, std::span<VkDescriptorSetAndBindingMappingEXT> out) const noexcept;

    [[nodiscard]] auto BufferAddress(VkBuffer buffer) const noexcept -> VkDeviceAddress {
        const VkBufferDeviceAddressInfo info {
            .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = buffer,
        };
        return vkGetBufferDeviceAddress(_resourceHeap.GetDevice(), &info);
    }

    template <typename Declared, typename... Slots>
    [[nodiscard]] auto WriteHeapParameters(const HeapPassBindings& b, const Slots&... slots) noexcept -> HeapBlockBase;

    [[nodiscard]] auto PushDataMaxSize() const noexcept -> VkDeviceSize {
        return _maxPushDataSize;
    }

    void BindHeaps(VkCommandBuffer cmd) const noexcept;

    [[nodiscard]] auto GetResourceHeapBindInfo() const noexcept -> VkBindHeapInfoEXT {
        return _resourceHeap.GetBindInfo();
    }
    [[nodiscard]] auto GetSamplerHeapBindInfo() const noexcept -> VkBindHeapInfoEXT {
        return _samplerHeap.GetBindInfo();
    }

  private:
    friend class HeapImmediateScope;
    friend class HeapFrameScope;

    [[nodiscard]] auto AllocateStaticResourceSlot() noexcept -> std::expected<uint32_t, ErrorCode>;
    void               FreeStaticResourceSlot(uint32_t slot) noexcept;
    [[nodiscard]] auto AllocateStaticSamplerSlot() noexcept -> std::expected<uint32_t, ErrorCode>;
    void               FreeStaticSamplerSlot(uint32_t slot) noexcept;

    [[nodiscard]] auto AllocateTransientResourceRange(uint32_t count, HeapLifecycle lifecycle) noexcept
        -> std::expected<uint32_t, ErrorCode>;

    void FlushResourceBatch(ResourceWriteBatch& batch) noexcept;
    void FlushSamplerBatch(SamplerWriteBatch& batch) noexcept;

    [[nodiscard]] auto ResourceStride() const noexcept -> VkDeviceSize {
        return _resourceHeap.GetStride();
    }
    [[nodiscard]] auto SamplerStride() const noexcept -> VkDeviceSize {
        return _samplerHeap.GetStride();
    }
    [[nodiscard]] auto ResourceOffset(uint32_t slot) const noexcept -> VkDeviceSize {
        return _resourceHeap.SlotOffset(slot);
    }
    [[nodiscard]] auto SamplerOffset(uint32_t slot) const noexcept -> VkDeviceSize {
        return _samplerHeap.SlotOffset(slot);
    }

    [[nodiscard]] auto EnterImmediateScope() noexcept -> uint32_t;
    void               LeaveImmediateScope(uint32_t baseCursor) noexcept;
    void               EnterFrame(uint32_t frameIndex) noexcept;
    void               LeaveFrame() noexcept;

    ZHLN::Mutex _writeMutex {};

    DescriptorHeap<DescriptorHeapType::Resources> _resourceHeap;
    DescriptorHeap<DescriptorHeapType::Samplers>  _samplerHeap;

    uint32_t _staticResourceCount             = 0;
    uint32_t _staticSamplerCount              = 0;
    uint32_t _frameTransientResourceCount     = 0;
    uint32_t _immediateTransientResourceCount = 0;
    uint32_t _currentFrameIndex               = 0;

    VkDeviceSize _maxPushDataSize = 0;

    SlotAllocator _staticResourceAlloc;
    SlotAllocator _staticSamplerAlloc;

    uint32_t _frameTransientAllocated     = 0;
    uint32_t _immediateTransientAllocated = 0;
};

/// RAII transaction over the immediate transient partition. Allocation inside the
/// scope bumps the partition; dropping the scope puts the cursor back where the
/// scope found it. Scopes are LIFO, so overlapping passes get disjoint slot
/// ranges instead of resetting each other's cursor to zero.
class HeapImmediateScope {
  public:
    explicit HeapImmediateScope(HeapManager& owner) noexcept;
    ~HeapImmediateScope() noexcept;

    HeapImmediateScope(const HeapImmediateScope&)                    = delete;
    auto operator=(const HeapImmediateScope&) -> HeapImmediateScope& = delete;

    HeapImmediateScope(HeapImmediateScope&& other) noexcept;
    auto operator=(HeapImmediateScope&& other) noexcept -> HeapImmediateScope&;

    template <typename Declared, typename... Slots>
    [[nodiscard]] auto WriteParameters(const HeapPassBindings& b, const Slots&... slots) noexcept -> HeapBlockBase {
        return _owner->template WriteHeapParameters<Declared>(b, slots...);
    }

    void Bind(VkCommandBuffer cmd) const noexcept {
        _owner->BindHeaps(cmd);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return _owner != nullptr;
    }

  private:
    void Close() noexcept;

    HeapManager* _owner      = nullptr;
    uint32_t     _baseCursor = 0;
};

/// RAII transaction for one frame's transient partition. The renderer's frame
/// scope opens it and drops it when the frame ends; nothing below it resets the
/// frame cursor.
class HeapFrameScope {
  public:
    HeapFrameScope(HeapManager& owner, uint32_t frameIndex) noexcept;
    ~HeapFrameScope() noexcept;

    HeapFrameScope(const HeapFrameScope&)                    = delete;
    auto operator=(const HeapFrameScope&) -> HeapFrameScope& = delete;

    HeapFrameScope(HeapFrameScope&& other) noexcept;
    auto operator=(HeapFrameScope&& other) noexcept -> HeapFrameScope&;

    [[nodiscard]] auto FrameIndex() const noexcept -> uint32_t {
        return _frameIndex;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return _owner != nullptr;
    }

  private:
    void Close() noexcept;

    HeapManager* _owner      = nullptr;
    uint32_t     _frameIndex = 0;
};

}
