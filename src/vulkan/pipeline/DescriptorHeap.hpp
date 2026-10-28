// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../memory/Allocator.hpp"
#include "../memory/BufferSlice.hpp"
#include "../memory/ImageView.hpp"
#include "DescriptorMapping.hpp"
#include "Sampler.hpp"

#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::Vk {

/// Descriptor heap state as a secondary command buffer sees it: two pointers to
/// binding state it must inherit, because a secondary does not inherit the
/// bindings of the primary it is executed in.
///
/// Both pointers address a DescriptorHeap's own `_bindInfo`, so they stay valid
/// for as long as the manager that produced them exists, and this type is cheap
/// enough to pass around while that is true.
class HeapInheritance {
  public:
    constexpr HeapInheritance() noexcept = default;

    constexpr HeapInheritance(const VkBindHeapInfoEXT* samplerBind, const VkBindHeapInfoEXT* resourceBind) noexcept:
        _samplerBind(samplerBind), _resourceBind(resourceBind) {
    }

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return _samplerBind != nullptr || _resourceBind != nullptr;
    }
    explicit constexpr operator bool() const noexcept {
        return Valid();
    }

    /// Tells the inheritance chain to link the descriptor heap state, and returns
    /// the head of the chain. `storage` is only read by the vkBeginCommandBuffer
    /// this feeds.
    [[nodiscard]] auto Link(void* pNext, VkCommandBufferInheritanceDescriptorHeapInfoEXT& storage) const noexcept -> void* {
        if (!Valid()) {
            return pNext;
        }
        storage = {
            .sType                 = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_DESCRIPTOR_HEAP_INFO_EXT,
            .pNext                 = pNext,
            .pSamplerHeapBindInfo  = _samplerBind,
            .pResourceHeapBindInfo = _resourceBind,
        };
        return &storage;
    }

  private:
    const VkBindHeapInfoEXT* _samplerBind  = nullptr;
    const VkBindHeapInfoEXT* _resourceBind = nullptr;
};

class HeapManager;
class HeapImmediateScope;
class HeapFrameScope;
struct HeapPassBindings;

enum class DescriptorHeapType : uint8_t {
    Resources,
    Samplers
};

enum class HeapLifecycle : uint8_t {
    Frame,
    Immediate
};

struct HeapConfig {
    uint32_t warningThresholdPercent = 75;
    bool     enableCapacityWarnings  = false;
};

enum class DescriptorHeapError : uint8_t {
    ExtensionUnavailable ZHLN_ANNOTATION(ZHLN::Description<"VK_EXT_descriptor_heap is not enabled on this device">{}) = 1,
    ResourceSlotsExhausted ZHLN_ANNOTATION(ZHLN::Description<"Static resource descriptor heap slots exhausted">{}),
    SamplerSlotsExhausted ZHLN_ANNOTATION(ZHLN::Description<"Static sampler descriptor heap slots exhausted">{}),
    TransientResourceOverflow ZHLN_ANNOTATION(ZHLN::Description<"Transient resource descriptor heap partition overflowed">{}),
    FunctionLoaderFailed ZHLN_ANNOTATION(ZHLN::Description<"VK_EXT_descriptor_heap entry points failed to load">{}),
    AllocationFailed ZHLN_ANNOTATION(ZHLN::Description<"Descriptor heap buffer allocation failed">{}),
    MappingFailed ZHLN_ANNOTATION(ZHLN::Description<"Descriptor heap mapping failed">{}),
    DeviceAddressFailed ZHLN_ANNOTATION(ZHLN::Description<"Descriptor heap buffer device address invalid or misaligned">{}),
    HeapTooLarge ZHLN_ANNOTATION(ZHLN::Description<"Requested descriptor heap exceeds device maxHeapSize">{}),
    NoActiveImmediateScope ZHLN_ANNOTATION(ZHLN::Description<"Immediate transient descriptor allocation requires an active immediate scope">{})
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

// -----------------------------------------------------------------------------
// Passive Descriptor Staging Batches
// -----------------------------------------------------------------------------
// Pure semantic input payloads: no Vulkan handles, no mapped pointers, no byte
// strides. DescriptorHeap consumes these directly and synthesizes the C ABI
// structs on the stack in bounded chunks when writing into its own memory.

struct ResourceWriteEntry {
    enum class Kind : uint8_t {
        Image,
        AddressRange,
    };

    uint32_t         slot   = 0;
    VkDescriptorType type   = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    Kind             kind   = Kind::AddressRange;
    VkImageLayout    layout = VK_IMAGE_LAYOUT_UNDEFINED;

    union Payload {
        ImageView               view;
        VkDeviceAddressRangeEXT range;

        constexpr Payload() noexcept: range {} {
        }
    } payload {};

    [[nodiscard]] static constexpr auto Image(uint32_t slot, VkDescriptorType type, const ImageView& view, VkImageLayout layout) noexcept
        -> ResourceWriteEntry {
        ResourceWriteEntry entry {
            .slot   = slot,
            .type   = type,
            .kind   = Kind::Image,
            .layout = layout,
        };
        entry.payload.view = view;
        return entry;
    }

    [[nodiscard]] static constexpr auto Address(uint32_t slot, VkDescriptorType type, VkDeviceAddressRangeEXT range) noexcept
        -> ResourceWriteEntry {
        ResourceWriteEntry entry {
            .slot = slot,
            .type = type,
            .kind = Kind::AddressRange,
        };
        entry.payload.range = range;
        return entry;
    }
};

static_assert(std::is_trivially_copyable_v<ResourceWriteEntry>);

class ResourceWriteBatch {
  public:
    ResourceWriteBatch() noexcept = default;

    void AddImage(TextureHandle handle, const ImageView& view, VkImageLayout layout) noexcept {
        if (handle.Valid() && view.Handle() != VK_NULL_HANDLE) {
            _entries.push_back(ResourceWriteEntry::Image(handle.index, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, view, layout));
        }
    }

    void AddStorageImage(StorageImageHandle handle, const ImageView& view, VkImageLayout layout) noexcept {
        if (handle.Valid() && view.Handle() != VK_NULL_HANDLE) {
            _entries.push_back(ResourceWriteEntry::Image(handle.index, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, view, layout));
        }
    }

    void AddBuffer(StorageBufferHandle handle, BufferSlice slice) noexcept {
        if (handle.Valid()) {
            _entries.push_back(
                ResourceWriteEntry::Address(handle.index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, {.address = slice.Address().Get(), .size = slice.Size()})
            );
        }
    }

    void AddBuffer(UniformBufferHandle handle, BufferSlice slice) noexcept {
        if (handle.Valid()) {
            _entries.push_back(
                ResourceWriteEntry::Address(handle.index, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, {.address = slice.Address().Get(), .size = slice.Size()})
            );
        }
    }

    void AddAccelerationStructure(AccelerationStructureHandle handle, VkDeviceAddress address) noexcept {
        if (handle.Valid()) {
            _entries.push_back(ResourceWriteEntry::Address(handle.index, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, {.address = address, .size = 0}));
        }
    }

    void Clear() noexcept {
        _entries.clear();
    }

    [[nodiscard]] auto Empty() const noexcept -> bool {
        return _entries.empty();
    }

    [[nodiscard]] auto Entries() const noexcept -> std::span<const ResourceWriteEntry> {
        return {_entries.data(), _entries.size()};
    }

  private:
    ZHLN::Array<ResourceWriteEntry, 16> _entries {};
};

struct SamplerWriteEntry {
    uint32_t slot = 0;
    Sampler  sampler {};
};

static_assert(std::is_trivially_copyable_v<SamplerWriteEntry>);

class SamplerWriteBatch {
  public:
    SamplerWriteBatch() noexcept = default;

    void AddSampler(SamplerHandle handle, Sampler sampler) noexcept {
        if (handle.Valid()) {
            _entries.push_back({.slot = handle.index, .sampler = sampler});
        }
    }

    void Clear() noexcept {
        _entries.clear();
    }

    [[nodiscard]] auto Empty() const noexcept -> bool {
        return _entries.empty();
    }

    [[nodiscard]] auto Entries() const noexcept -> std::span<const SamplerWriteEntry> {
        return {_entries.data(), _entries.size()};
    }

  private:
    ZHLN::Array<SamplerWriteEntry, 16> _entries {};
};

// -----------------------------------------------------------------------------
// DescriptorHeap
// -----------------------------------------------------------------------------
// Owns a `VK_EXT_descriptor_heap` device-addressable buffer AND its slot
// partitions, descriptor writes, non-coherent cache flushes, and pipeline
// mapping offset resolution. Hardware strides, byte offsets, and mapped host
// addresses never leave this class.
class DescriptorHeap {
  public:
    struct PartitionLayout {
        uint32_t staticSlots             = 0;
        uint32_t frameTransientSlots     = 0;
        uint32_t immediateTransientSlots = 0;
    };

    /// RAII capability token over the immediate transient partition. Minted
    /// exclusively by `DescriptorHeap::CreateImmediateScope()`; `EnterImmediate`
    /// and `LeaveImmediate` stay private so callers cannot pass forged or stale
    /// cursors. Closing scopes out of LIFO order defers cursor rewind until all
    /// newer overlapping scopes have also closed, avoiding UB or live-slot reuse.
    class ImmediateScope {
      public:
        ImmediateScope() noexcept = default;
        ~ImmediateScope() noexcept;

        ImmediateScope(const ImmediateScope&)                    = delete;
        auto operator=(const ImmediateScope&) -> ImmediateScope& = delete;

        ImmediateScope(ImmediateScope&& other) noexcept;
        auto operator=(ImmediateScope&& other) noexcept -> ImmediateScope&;

        void Reset() noexcept;

        [[nodiscard]] explicit operator bool() const noexcept {
            return _heap != nullptr;
        }

      private:
        friend class DescriptorHeap;

        ImmediateScope(DescriptorHeap& heap, uint64_t scopeId) noexcept: _heap(&heap), _scopeId(scopeId) {
        }

        DescriptorHeap* _heap    = nullptr;
        uint64_t        _scopeId = 0;
    };

    DescriptorHeap() = default;
    ~DescriptorHeap() noexcept;

    DescriptorHeap(const DescriptorHeap&)                    = delete;
    auto operator=(const DescriptorHeap&) -> DescriptorHeap& = delete;

    DescriptorHeap(DescriptorHeap&& other) noexcept;
    auto operator=(DescriptorHeap&& other) noexcept -> DescriptorHeap&;

    [[nodiscard]] auto Init(
        const Context& ctx, Allocator& allocator, DescriptorHeapType type, PartitionLayout layout, HeapConfig config = {}
    ) noexcept -> std::expected<void, ErrorCode>;
    void Cleanup() noexcept;

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _buffer.Valid();
    }
    explicit operator bool() const noexcept {
        return Valid();
    }

    // -------------------------------------------------------------------------
    // Command Binding & Secondary Inheritance
    // -------------------------------------------------------------------------
    void Bind(VkCommandBuffer cmd) const noexcept;

    [[nodiscard]] auto BindInfo() const noexcept -> const VkBindHeapInfoEXT* {
        return Valid() ? &_bindInfo : nullptr;
    }

    // -------------------------------------------------------------------------
    // Slot Allocation & Partition Lifecycles
    // -------------------------------------------------------------------------
    [[nodiscard]] auto AllocateStatic() noexcept -> std::expected<uint32_t, ErrorCode>;
    [[nodiscard]] auto AllocateStaticRange(uint32_t count) noexcept -> std::expected<uint32_t, ErrorCode>;
    void               FreeStatic(uint32_t slot) noexcept;

    void               BeginFrame(uint32_t frameIndex) noexcept;
    [[nodiscard]] auto CreateImmediateScope() noexcept -> ImmediateScope;
    [[nodiscard]] auto AllocateTransientRange(uint32_t count, HeapLifecycle lifecycle) noexcept -> std::expected<uint32_t, ErrorCode>;

    // -------------------------------------------------------------------------
    // Descriptor Writing (Heap owns memory addressing, driver writes & flushes)
    // -------------------------------------------------------------------------
    void Write(const ResourceWriteBatch& batch) noexcept;
    void Write(const SamplerWriteBatch& batch) noexcept;
    void WriteResource(const ResourceWriteEntry& entry) noexcept;
    void WriteSampler(SamplerHandle handle, Sampler sampler) noexcept;

    // -------------------------------------------------------------------------
    // Pipeline Mapping Resolution (Heap resolves slot -> byte offset/stride)
    // -------------------------------------------------------------------------
    [[nodiscard]] auto ResolveMapping(const DescriptorMapping& mapping, const DescriptorHeap* pairedSamplerHeap = nullptr) const noexcept
        -> VkDescriptorSetAndBindingMappingEXT;

  private:
    friend class ImmediateScope;

    struct ImmediateCheckpoint {
        uint64_t id         = 0;
        uint32_t baseCursor = 0;
        bool     closed     = false;
    };

    [[nodiscard]] auto EnterImmediate() noexcept -> uint64_t;
    void               LeaveImmediate(uint64_t scopeId) noexcept;
    void               WriteResources(std::span<const ResourceWriteEntry> entries) noexcept;
    void               WriteSamplers(std::span<const SamplerWriteEntry> entries) noexcept;
    [[nodiscard]] auto ValidateWriteSlot(uint32_t slot, uint32_t batchCount) const noexcept -> bool;
    [[nodiscard]] auto IsSlotWritable(uint32_t slot) const noexcept -> bool;
    void               MarkStaticLive(uint32_t slot) noexcept;
    void               ClearStaticLive(uint32_t slot) noexcept;
    [[nodiscard]] auto IsStaticLive(uint32_t slot) const noexcept -> bool;
    void               FlushHostCache(VkDeviceSize offset, VkDeviceSize size) noexcept;

    VkDevice           _device = VK_NULL_HANDLE;
    DescriptorHeapType _type   = DescriptorHeapType::Resources;
    HeapConfig         _config {};

    uint32_t _capacity                    = 0;
    uint32_t _staticCapacity              = 0;
    uint32_t _staticCursor                = 0;
    uint32_t _frameTransientCapacity      = 0;
    uint32_t _currentFrameSlot            = 0;
    uint32_t _frameTransientAllocated     = 0;
    uint32_t _immediateTransientCapacity  = 0;
    uint32_t _immediateTransientAllocated = 0;

    VkDeviceSize _stride              = 0;
    VkDeviceSize _reservedSize        = 0;
    VkDeviceSize _nonCoherentAtomSize = 1;

    Allocator*           _allocator = nullptr;
    Buffer               _buffer;
    Buffer::MappedRegion _mappedRegion;
    void*                _mappedPtr = nullptr;
    VkBindHeapInfoEXT    _bindInfo  = {};

    uint64_t                            _nextImmediateScopeId = 0;
    ZHLN::Array<ImmediateCheckpoint, 8> _immediateScopes {};
    std::vector<uint32_t>               _freeStaticSlots;
    std::vector<uint64_t>               _liveStaticBits;
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
        uint32_t       immediateTransientResourceCount,
        HeapConfig     config = {}
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
    [[nodiscard]] auto RegisterSampler(Sampler sampler) noexcept -> std::expected<SamplerHandle, ErrorCode>;

    // `SamplerAt(i)` builds the i-th variant, so the caller describes the array
    // (the nine material address-mode combinations) instead of materializing one.
    template <typename SamplerAt>
    [[nodiscard]] auto RegisterSamplerArray(uint32_t count, SamplerAt&& samplerAt) noexcept -> std::expected<SamplerHandle, ErrorCode> {
        auto base = ReserveSamplerRange(count);
        if (!base) [[unlikely]] {
            return base;
        }
        SamplerWriteBatch batch;
        for (uint32_t i = 0; i < count; ++i) {
            batch.AddSampler(SamplerHandle {base->index + i}, samplerAt(i));
        }
        WriteSamplers(batch);
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
    void BindSampler(SamplerHandle handle, Sampler sampler) noexcept;
    void BindImage(TextureHandle handle, const ImageView& view, VkImageLayout layout) noexcept;
    void BindStorageImage(StorageImageHandle handle, const ImageView& view, VkImageLayout layout) noexcept;
    void BindStorageBuffer(StorageBufferHandle handle, BufferSlice slice) noexcept;
    void BindUniformBuffer(UniformBufferHandle handle, BufferSlice slice) noexcept;
    void BindAccelerationStructure(AccelerationStructureHandle handle, VkDeviceAddress address) noexcept;
    void WriteResources(const ResourceWriteBatch& batch) noexcept;
    void WriteSamplers(const SamplerWriteBatch& batch) noexcept;

    void ReleaseSampler(SamplerHandle handle) noexcept;
    template <VkDescriptorType Type>
        requires ValidResourceDescriptorType<Type>
    void ReleaseResource(HeapHandle<DescriptorHeapType::Resources, Type> handle) noexcept {
        if (handle.Valid()) {
            FreeStaticResourceSlot(handle.index);
        }
    }

    // -------------------------------------------------------------------------
    // Descriptor mapping. DescriptorHeap owns its stride, padding and partition
    // layout; these append semantic entries naming slots, and Materialize()
    // delegates byte-offset resolution to the target heap.
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

    /// The API boundary: tells each DescriptorHeap to materialize its semantic
    /// `mappings` into `out` as the driver struct.
    void Materialize(HeapMappingView mappings, std::span<VkDescriptorSetAndBindingMappingEXT> out) const noexcept;

    [[nodiscard]] auto BufferAddress(VkBuffer buffer) const noexcept -> VkDeviceAddress {
        if (_device == VK_NULL_HANDLE || buffer == VK_NULL_HANDLE) {
            return 0;
        }
        const VkBufferDeviceAddressInfo info {
            .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = buffer,
        };
        return vkGetBufferDeviceAddress(_device, &info);
    }

    template <typename Declared, typename... Slots>
    [[nodiscard]] auto WriteHeapParameters(const HeapPassBindings& b, const Slots&... slots) noexcept -> HeapBlockBase;

    [[nodiscard]] auto PushDataMaxSize() const noexcept -> VkDeviceSize {
        return _maxPushDataSize;
    }

    void BindHeaps(VkCommandBuffer cmd) const noexcept;

    /// A handle on where the heap binding state lives, so a secondary command
    /// buffer can inherit it.
    [[nodiscard]] auto GetInheritance() const noexcept -> HeapInheritance {
        return HeapInheritance {_samplerHeap.BindInfo(), _resourceHeap.BindInfo()};
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

    [[nodiscard]] auto EnterImmediateScope() noexcept -> DescriptorHeap::ImmediateScope;
    void               LeaveImmediateScope(DescriptorHeap::ImmediateScope&& scope) noexcept;
    void               EnterFrame(uint32_t frameIndex) noexcept;
    void               LeaveFrame() noexcept;

    ZHLN::Mutex  _writeMutex {};
    VkDevice     _device          = VK_NULL_HANDLE;
    VkDeviceSize _maxPushDataSize = 0;

    DescriptorHeap _resourceHeap;
    DescriptorHeap _samplerHeap;
};

/// RAII transaction over the immediate transient partition. Allocation inside the
/// scope bumps the partition; dropping the scope puts the cursor back where the
/// scope found it. Overlapping scopes get disjoint slot ranges instead of
/// resetting each other's cursor to zero.
class HeapImmediateScope {
  public:
    HeapImmediateScope() noexcept = default;
    ~HeapImmediateScope() noexcept;

    HeapImmediateScope(const HeapImmediateScope&)                    = delete;
    auto operator=(const HeapImmediateScope&) -> HeapImmediateScope& = delete;

    HeapImmediateScope(HeapImmediateScope&& other) noexcept;
    auto operator=(HeapImmediateScope&& other) noexcept -> HeapImmediateScope&;

    template <typename Declared, typename... Slots>
    [[nodiscard]] auto WriteParameters(const HeapPassBindings& b, const Slots&... slots) noexcept -> HeapBlockBase {
        if (_owner == nullptr) [[unlikely]] {
            return {};
        }
        return _owner->template WriteHeapParameters<Declared>(b, slots...);
    }

    void Bind(VkCommandBuffer cmd) const noexcept {
        if (_owner != nullptr) {
            _owner->BindHeaps(cmd);
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return _owner != nullptr;
    }

  private:
    friend class HeapManager;

    explicit HeapImmediateScope(HeapManager& owner) noexcept;
    void Close() noexcept;

    HeapManager*                   _owner = nullptr;
    DescriptorHeap::ImmediateScope _scope {};
};

/// RAII transaction for one frame's transient partition. The renderer's frame
/// scope opens it and drops it when the frame ends; nothing below it resets the
/// frame cursor.
class HeapFrameScope {
  public:
    HeapFrameScope() noexcept = default;
    ~HeapFrameScope() noexcept;

    HeapFrameScope(const HeapFrameScope&)                    = delete;
    auto operator=(const HeapFrameScope&) -> HeapFrameScope& = delete;

    HeapFrameScope(HeapFrameScope&& other) noexcept;
    auto operator=(HeapFrameScope&& other) noexcept -> HeapFrameScope&;

    [[nodiscard]] explicit operator bool() const noexcept {
        return _owner != nullptr;
    }

  private:
    friend class HeapManager;

    HeapFrameScope(HeapManager& owner, uint32_t frameIndex) noexcept;
    void Close() noexcept;

    HeapManager* _owner      = nullptr;
    uint32_t     _frameIndex = 0;
};

} // namespace ZHLN::Vk
