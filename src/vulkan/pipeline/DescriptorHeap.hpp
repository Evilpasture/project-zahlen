// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/DescriptorHeap.hpp
//
// VK_EXT_descriptor_heap backing infrastructure.
//
// Model: the engine owns ONE sampler heap and ONE resource heap, each backed
// by a single host-visible, persistently mapped, device-addressable buffer
// created with VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT. Descriptors are not
// Vulkan objects: they are opaque bit patterns produced on the host by
// vkWriteResourceDescriptorsEXT / vkWriteSamplerDescriptorsEXT and written
// into the heap buffer at a slot-aligned offset. Command buffers see the
// heaps after vkCmdBindResourceHeapEXT / vkCmdBindSamplerHeapEXT.
//
// Slot layout (resource heap): every slot uses one unified stride
//   stride = Math::AlignUp(max(bufferDescriptorSize, imageDescriptorSize),
//                          max(bufferDescriptorAlignment, imageDescriptorAlignment))
// so any descriptor type fits any slot and the spec's alignment VUIDs for
// both the write ranges and reservedRangeOffset hold.
//
// The tail of each heap buffer is reserved for the implementation
// (minResourceHeapReservedRange / minSamplerHeapReservedRange); the
// reservedRangeOffset/reservedRangeSize fields of VkBindHeapInfoEXT point at
// it and the application must never touch it while bound.

#pragma once
#include <Zahlen/Threading/Mutex.hpp>

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

// Forward declarations to break inline dependency loops
class ResourceWriteBatch;
class SamplerWriteBatch;
class HeapManager;
struct HeapPassBindings;

// Plural on purpose: a singular `Sampler` enumerator shadowed the
// ZHLN::Vk::Sampler device-handle alias from Handles.hpp under -Wshadow.
enum class DescriptorHeapType : uint8_t {
    Resources, // Storage Buffers, Uniform Buffers, Sampled Images, Storage Images, AS
    Samplers   // Samplers only
};

/// Which transient partition a pass's binding blocks are allocated from. A
/// lifecycle, not a tuning knob: a pass recorded inside the frame loop cannot
/// share blocks with work submitted outside it.
enum class HeapLifecycle : uint8_t {
    /// Recorded while the frame is being recorded. Blocks come from the
    /// partition of the frame being recorded (one per frame parity, so a block
    /// stays untouched while the previous frame is still executing), and the
    /// partition is rewound by BeginFrame.
    Frame,
    /// Submitted and completed outside the frame loop (ExecuteImmediate: the
    /// texture bakes). Blocks come from a separate partition, rewound by
    /// BeginImmediate.
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

// ============================================================================
// Strongly-Typed Semantic Aliases
// ============================================================================
using TextureHandle               = HeapHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>;
using StorageImageHandle          = HeapHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE>;
using UniformBufferHandle         = HeapHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER>;
using StorageBufferHandle         = HeapHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER>;
using SamplerHandle               = HeapHandle<DescriptorHeapType::Samplers, VK_DESCRIPTOR_TYPE_SAMPLER>;
using AccelerationStructureHandle = HeapHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR>;

// ============================================================================
// Concepts
// ============================================================================
template <VkDescriptorType Type>
concept ValidResourceDescriptorType = Type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || Type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                                      Type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || Type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                                      Type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

// ============================================================================
// Global Invalid Constants
// ============================================================================
inline constexpr TextureHandle               kInvalidTextureHandle       = kInvalidHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>;
inline constexpr StorageImageHandle          kInvalidStorageImageHandle  = kInvalidHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE>;
inline constexpr UniformBufferHandle         kInvalidUniformBufferHandle = kInvalidHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER>;
inline constexpr StorageBufferHandle         kInvalidStorageBufferHandle = kInvalidHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER>;
inline constexpr SamplerHandle               kInvalidSamplerHandle       = kInvalidHandle<DescriptorHeapType::Samplers, VK_DESCRIPTOR_TYPE_SAMPLER>;
inline constexpr AccelerationStructureHandle kInvalidAccelerationStructureHandle =
    kInvalidHandle<DescriptorHeapType::Resources, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR>;

// Validate layout safety at compile time
static_assert(sizeof(TextureHandle) == sizeof(uint32_t));
static_assert(std::is_standard_layout_v<TextureHandle>);
static_assert(std::is_trivially_copyable_v<TextureHandle>);

// ============================================================================
// Descriptor Heap Abstraction
// ============================================================================

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

    /// Binds this heap to a command buffer. Recording this invalidates all
    /// legacy descriptor-set and push-constant state (and vice versa).
    void Bind(VkCommandBuffer cmd) const noexcept;

    /// The cached VkBindHeapInfoEXT for this heap (address/size/reserved
    /// range). Secondary command buffers chain it into
    /// VkCommandBufferInheritanceDescriptorHeapInfoEXT to inherit the
    /// primary's binding.
    [[nodiscard]] auto GetBindInfo() const noexcept -> VkBindHeapInfoEXT {
        return _bindInfo;
    }

    // Enforce C++ type safety with compile-time template constraints
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

    /// Byte offset of a slot inside the heap (what the shader mappings use).
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

    Buffer               _buffer;
    Buffer::MappedRegion _mappedRegion;
    void*                _mappedPtr = nullptr;
    VkBindHeapInfoEXT    _bindInfo  = {};
};

// ============================================================================
// Zero-Allocation Write Batch Processors (PIMPL)
// ============================================================================

class ResourceWriteBatch {
  public:
    ResourceWriteBatch() noexcept;
    ~ResourceWriteBatch() noexcept;

    ResourceWriteBatch(const ResourceWriteBatch&)                    = delete;
    auto operator=(const ResourceWriteBatch&) -> ResourceWriteBatch& = delete;

    ResourceWriteBatch(ResourceWriteBatch&& other) noexcept;
    auto operator=(ResourceWriteBatch&& other) noexcept -> ResourceWriteBatch&;

    // Overloaded typed write commands
    void AddImage(TextureHandle handle, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout) noexcept;
    void AddStorageImage(StorageImageHandle handle, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout) noexcept;
    void AddBuffer(StorageBufferHandle handle, VkDeviceAddress address, VkDeviceSize size) noexcept;
    void AddBuffer(UniformBufferHandle handle, VkDeviceAddress address, VkDeviceSize size) noexcept;
    void AddAccelerationStructure(AccelerationStructureHandle handle, VkDeviceAddress address) noexcept;

    void Flush(VkDevice device, PFN_vkWriteResourceDescriptorsEXT writeFn, void* mappedPtr, VkDeviceSize stride) noexcept;

    [[nodiscard]] auto Empty() const noexcept -> bool;
    [[nodiscard]] auto SlotCount() const noexcept -> uint32_t;
    [[nodiscard]] auto SlotsData() const noexcept -> const uint32_t*;

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

    void AddSampler(SamplerHandle handle, const VkSamplerCreateInfo& createInfo) noexcept;

    void Flush(VkDevice device, PFN_vkWriteSamplerDescriptorsEXT writeFn, void* mappedPtr, VkDeviceSize stride) noexcept;

    [[nodiscard]] auto Empty() const noexcept -> bool;
    [[nodiscard]] auto SlotCount() const noexcept -> uint32_t;
    [[nodiscard]] auto SlotsData() const noexcept -> const uint32_t*;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

// ============================================================================
// Slot Allocation Helper for Static Heaps (PIMPL)
// ============================================================================

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

// ============================================================================
// Single-Heap Partitioned Manager
// ============================================================================

class HeapManager {
  public:
    HeapManager()           = default;
    ~HeapManager() noexcept = default;

    HeapManager(const HeapManager&)                    = delete;
    auto operator=(const HeapManager&) -> HeapManager& = delete;

    HeapManager(HeapManager&&) noexcept                    = default;
    auto operator=(HeapManager&&) noexcept -> HeapManager& = default;

    /// Creates both heaps. Layout:
    ///   [0, staticResourceCount)                        static resource slots
    ///   [staticResourceCount, +frameTransient*buffers)  per-frame transient blocks
    ///   [.., +immediateTransient)                       out-of-frame transient blocks
    /// with the sampler heap holding static slots only (a sampler binding is
    /// addressed at a constant heap offset, so it cannot travel per dispatch).
    /// The tail of each buffer holds the implementation-reserved range.
    [[nodiscard]] auto Init(
        const Context& ctx,
        Allocator&     allocator,
        uint32_t       staticResourceCount,
        uint32_t       staticSamplerCount,
        uint32_t       frameTransientResourceCount,
        uint32_t       immediateTransientResourceCount,
        uint32_t       doubleBufferCount = 2
    ) noexcept -> std::expected<void, ErrorCode>;

    /// Rewinds the frame transient partition for `frameIndex`'s recording. Every
    /// block handed out before the next BeginFrame belongs to that frame.
    ///
    /// Threading: the transient partitions and the mapped heap buffers are
    /// shared by every thread that records a frame, because Vk::Fork records
    /// its sub-passes concurrently. Allocation and every host-side descriptor
    /// write take `_writeMutex`, which is what keeps two forked passes from
    /// being handed overlapping blocks. Recording bodies must never hold the
    /// lock across their own work: each allocation and each write is a
    /// separate, short critical section.
    void BeginFrame(uint32_t frameIndex) noexcept;

    /// Rewinds the immediate transient partition. Callers must have completed
    /// the previous immediate submission (ExecuteImmediate's default
    /// blockCPU=true does), because nothing else keeps those blocks alive.
    void BeginImmediate() noexcept;

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _resourceHeap.Valid() && _samplerHeap.Valid();
    }

    // Reserves a region that is addressed by offset instead of by
    // allocator-issued slots (the bindless globalTextures[] array and the
    // unused headroom beside the scene registry slots). The returned base is
    // what the caller's mapping points at; no heap slot is ever handed out from
    // inside the region, and because the base travels back to the caller, a
    // stray allocation before the reservation moves the region rather than
    // silently overlapping it.
    [[nodiscard]] auto ReserveOffsetAddressedResourceRegion(uint32_t count) noexcept -> std::expected<uint32_t, ErrorCode>;

    // --- Type-Safe Static Resource Allocation ---
    template <VkDescriptorType Type>
        requires ValidResourceDescriptorType<Type>
    [[nodiscard]] auto AllocateStaticResource() noexcept -> std::expected<HeapHandle<DescriptorHeapType::Resources, Type>, ErrorCode> {
        return AllocateStaticResourceSlot().transform([](uint32_t idx) { return HeapHandle<DescriptorHeapType::Resources, Type> {idx}; });
    }

    [[nodiscard]] auto AllocateStaticSampler() noexcept -> std::expected<SamplerHandle, ErrorCode> {
        return AllocateStaticSamplerSlot().transform([](uint32_t idx) { return SamplerHandle {idx}; });
    }

    // --- Transient Range Allocation ---
    /// Reserves `count` contiguous resource slots in `lifecycle`'s current
    /// partition and returns the base slot. Blocks are bump-allocated: order
    /// within a partition is the order the writes happen, and the whole
    /// partition is rewound at the top of the next frame (or immediate
    /// sequence), which is what makes the blocks transient. Overflow means the
    /// partition is undersized -- a sizing bug the callers assert on.
    [[nodiscard]] auto AllocateTransientResourceRange(uint32_t count, HeapLifecycle lifecycle) noexcept
        -> std::expected<uint32_t, ErrorCode>;

    // --- Type-Safe Static Reclamation ---
    template <VkDescriptorType Type>
    void FreeStaticResource(HeapHandle<DescriptorHeapType::Resources, Type> handle) noexcept {
        FreeStaticResourceSlot(handle.index);
    }

    void FreeStaticSampler(SamplerHandle handle) noexcept {
        FreeStaticSamplerSlot(handle.index);
    }

    // --- Host-Side Descriptor Writes (immediately flushed into the heap) ---
    void WriteImage(TextureHandle handle, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout) noexcept;
    void WriteStorageImage(StorageImageHandle handle, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout) noexcept;
    void WriteBuffer(StorageBufferHandle handle, VkDeviceAddress address, VkDeviceSize size) noexcept;
    void WriteBuffer(UniformBufferHandle handle, VkDeviceAddress address, VkDeviceSize size) noexcept;
    void WriteAccelerationStructure(AccelerationStructureHandle handle, VkDeviceAddress address) noexcept;
    void WriteSampler(SamplerHandle handle, const VkSamplerCreateInfo& createInfo) noexcept;

    /// Writes one descriptor per argument (Vk::Slot<"binding">(value),
    /// DescriptorWrites.hpp) into a fresh transient block and returns its base
    /// (Vk::HeapBlockBase), which the dispatch pushes into the mapping's index
    /// word. Each name is matched against the binding names reflected for that
    /// set, so argument order carries no meaning; a value that cannot supply the
    /// binding's reflected descriptor type, a binding left unnamed, a binding
    /// named twice and an undersized partition all assert in dev builds. See
    /// HeapBindings.hpp for the walk.
    /// `Declared` is the pass's descriptor block (the generated <ShaderBindings.hpp>), which
    /// every name is checked against at compile time; see HeapBindings.hpp.
    template <typename Declared, typename... Slots>
    [[nodiscard]] auto WriteHeapParameters(const Context& ctx, const HeapPassBindings& b, const Slots&... slots) noexcept -> HeapBlockBase;

    void FlushResourceBatch(ResourceWriteBatch& batch) noexcept;
    void FlushSamplerBatch(SamplerWriteBatch& batch) noexcept;

    // --- Mapping Support (VkDescriptorSetAndBindingMappingEXT) ---
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
    [[nodiscard]] auto PushDataMaxSize() const noexcept -> VkDeviceSize {
        return _maxPushDataSize;
    }

    // --- Command Binding ---
    void BindHeaps(VkCommandBuffer cmd) const noexcept;

    // Cached bind descriptors for secondary-command-buffer inheritance.
    [[nodiscard]] auto GetResourceHeapBindInfo() const noexcept -> VkBindHeapInfoEXT {
        return _resourceHeap.GetBindInfo();
    }
    [[nodiscard]] auto GetSamplerHeapBindInfo() const noexcept -> VkBindHeapInfoEXT {
        return _samplerHeap.GetBindInfo();
    }

  private:
    [[nodiscard]] auto AllocateStaticResourceSlot() noexcept -> std::expected<uint32_t, ErrorCode>;
    void               FreeStaticResourceSlot(uint32_t slot) noexcept;
    [[nodiscard]] auto AllocateStaticSamplerSlot() noexcept -> std::expected<uint32_t, ErrorCode>;
    void               FreeStaticSamplerSlot(uint32_t slot) noexcept;

    /// Serializes transient block allocation and the host descriptor writes that
    /// fill those blocks; see the threading note on BeginFrame. Value-initialized
    /// on purpose: ZHLN::Mutex carries no default member initializer, so `{}` is
    /// what zeroes the byte it guards on.
    ZHLN::Mutex _writeMutex {};

    DescriptorHeap<DescriptorHeapType::Resources> _resourceHeap;
    DescriptorHeap<DescriptorHeapType::Samplers>  _samplerHeap;

    uint32_t _staticResourceCount             = 0;
    uint32_t _staticSamplerCount              = 0;
    uint32_t _frameTransientResourceCount     = 0;
    uint32_t _immediateTransientResourceCount = 0;
    uint32_t _doubleBufferCount               = 2;
    uint32_t _currentFrameIndex               = 0;

    VkDeviceSize _maxPushDataSize = 0;

    SlotAllocator _staticResourceAlloc;
    SlotAllocator _staticSamplerAlloc;

    uint32_t _frameTransientAllocated     = 0;
    uint32_t _immediateTransientAllocated = 0;
};

} // namespace ZHLN::Vk
