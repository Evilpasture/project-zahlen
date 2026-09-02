// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/DescriptorHeap.hpp
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
//   stride = AlignUp(max(bufferDescriptorSize, imageDescriptorSize),
//                    max(bufferDescriptorAlignment, imageDescriptorAlignment))
// so any descriptor type fits any slot and the spec's alignment VUIDs for
// both the write ranges and reservedRangeOffset hold.
//
// The tail of each heap buffer is reserved for the implementation
// (minResourceHeapReservedRange / minSamplerHeapReservedRange); the
// reservedRangeOffset/reservedRangeSize fields of VkBindHeapInfoEXT point at
// it and the application must never touch it while bound.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

// Forward declarations to break inline dependency loops
class ResourceWriteBatch;
class SamplerWriteBatch;
class HeapManager;
struct HeapPassBindings;

enum class DescriptorHeapType : uint8_t {
    Resource, // Storage Buffers, Uniform Buffers, Sampled Images, Storage Images, AS
    Sampler   // Samplers only
};

enum class DescriptorHeapError : uint8_t {
    ExtensionUnavailable = 1,
    ResourceSlotsExhausted,
    SamplerSlotsExhausted,
    DynamicResourceOverflow,
    DynamicSamplerOverflow,
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
using TextureHandle               = HeapHandle<DescriptorHeapType::Resource, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>;
using StorageImageHandle          = HeapHandle<DescriptorHeapType::Resource, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE>;
using UniformBufferHandle         = HeapHandle<DescriptorHeapType::Resource, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER>;
using StorageBufferHandle         = HeapHandle<DescriptorHeapType::Resource, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER>;
using SamplerHandle               = HeapHandle<DescriptorHeapType::Sampler, VK_DESCRIPTOR_TYPE_SAMPLER>;
using AccelerationStructureHandle = HeapHandle<DescriptorHeapType::Resource, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR>;

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
inline constexpr TextureHandle               kInvalidTextureHandle       = kInvalidHandle<DescriptorHeapType::Resource, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>;
inline constexpr StorageImageHandle          kInvalidStorageImageHandle  = kInvalidHandle<DescriptorHeapType::Resource, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE>;
inline constexpr UniformBufferHandle         kInvalidUniformBufferHandle = kInvalidHandle<DescriptorHeapType::Resource, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER>;
inline constexpr StorageBufferHandle         kInvalidStorageBufferHandle = kInvalidHandle<DescriptorHeapType::Resource, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER>;
inline constexpr SamplerHandle               kInvalidSamplerHandle       = kInvalidHandle<DescriptorHeapType::Sampler, VK_DESCRIPTOR_TYPE_SAMPLER>;
inline constexpr AccelerationStructureHandle kInvalidAccelerationStructureHandle =
    kInvalidHandle<DescriptorHeapType::Resource, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR>;

// Validate layout safety at compile time
static_assert(sizeof(TextureHandle) == sizeof(uint32_t));
static_assert(std::is_standard_layout_v<TextureHandle>);
static_assert(std::is_trivially_copyable_v<TextureHandle>);

// ============================================================================
// Alignment Helper
// ============================================================================

template <typename T, typename U>
[[nodiscard]] constexpr auto AlignUp(T value, U alignment) noexcept -> T {
    return (value + alignment - 1) & ~(alignment - 1);
}

template <typename T, typename U>
[[nodiscard]] constexpr auto AlignDown(T value, U alignment) noexcept -> T {
    return value & ~(alignment - 1);
}

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

    [[nodiscard]] auto Init(const Context& ctx, Allocator& allocator, uint32_t capacity) noexcept -> std::expected<void, Error>;
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
        requires(Type == DescriptorHeapType::Resource);
    void Flush(SamplerWriteBatch& batch) noexcept
        requires(Type == DescriptorHeapType::Sampler);

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

    // Compile-time conditional members via Type matching
    using BindHeapFn  = std::conditional_t<Type == DescriptorHeapType::Sampler, PFN_vkCmdBindSamplerHeapEXT, PFN_vkCmdBindResourceHeapEXT>;
    using WriteDescFn = std::conditional_t<Type == DescriptorHeapType::Sampler, PFN_vkWriteSamplerDescriptorsEXT, PFN_vkWriteResourceDescriptorsEXT>;

    BindHeapFn  _vkCmdBindHeapEXT      = nullptr;
    WriteDescFn _vkWriteDescriptorsEXT = nullptr;
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

    void               Init(uint32_t capacity, Error errorOnExhaustion) noexcept;
    [[nodiscard]] auto Allocate() noexcept -> std::expected<uint32_t, Error>;
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
    ///   [0, staticResourceCount)               static resource slots
    ///   [staticResourceCount, +dynamic*double) per-frame dynamic resource slots
    /// with an identical partition for the sampler heap. The tail of each
    /// buffer holds the implementation-reserved range.
    [[nodiscard]] auto Init(
        const Context& ctx,
        Allocator&     allocator,
        uint32_t       staticResourceCount,
        uint32_t       dynamicResourceCount,
        uint32_t       staticSamplerCount,
        uint32_t       dynamicSamplerCount,
        uint32_t       doubleBufferCount = 2
    ) noexcept -> std::expected<void, Error>;

    void BeginFrame(uint32_t frameIndex) noexcept;

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _resourceHeap.Valid() && _samplerHeap.Valid();
    }

    // Advances the static allocator cursors past reserved regions (e.g. the
    // bindless globalTextures[] array that is addressed by offset, not by
    // allocator-issued slots).
    void SkipStaticResourceSlots(uint32_t count) noexcept {
        _staticResourceAlloc.Skip(count);
    }
    void SkipStaticSamplerSlots(uint32_t count) noexcept {
        _staticSamplerAlloc.Skip(count);
    }
    [[nodiscard]] auto StaticResourceCursor() const noexcept -> uint32_t;
    [[nodiscard]] auto StaticSamplerCursor() const noexcept -> uint32_t;

    // --- Type-Safe Static Resource Allocation ---
    template <VkDescriptorType Type>
        requires ValidResourceDescriptorType<Type>
    [[nodiscard]] auto AllocateStaticResource() noexcept -> std::expected<HeapHandle<DescriptorHeapType::Resource, Type>, Error> {
        return AllocateStaticResourceSlot().transform([](uint32_t idx) { return HeapHandle<DescriptorHeapType::Resource, Type> {idx}; });
    }

    [[nodiscard]] auto AllocateStaticSampler() noexcept -> std::expected<SamplerHandle, Error> {
        return AllocateStaticSamplerSlot().transform([](uint32_t idx) { return SamplerHandle {idx}; });
    }

    // --- Type-Safe Dynamic Range Allocation ---
    template <VkDescriptorType Type>
        requires ValidResourceDescriptorType<Type>
    [[nodiscard]] auto
        AllocateDynamicResourceRange(uint32_t count) noexcept -> std::expected<HeapHandle<DescriptorHeapType::Resource, Type>, Error> {
        return AllocateDynamicResourceRangeSlot(count).transform([](uint32_t idx) { return HeapHandle<DescriptorHeapType::Resource, Type> {idx}; });
    }

    [[nodiscard]] auto AllocateDynamicSamplerRange(uint32_t count) noexcept -> std::expected<SamplerHandle, Error> {
        return AllocateDynamicSamplerRangeSlot(count).transform([](uint32_t idx) { return SamplerHandle {idx}; });
    }

    // --- Type-Safe Static Reclamation ---
    template <VkDescriptorType Type>
    void FreeStaticResource(HeapHandle<DescriptorHeapType::Resource, Type> handle) noexcept {
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

    /// Writes every non-sampler binding of `b` at `index`. Argument order
    /// mirrors the reflected set; sampler positions are skipped.
    template <typename... Args>
    void WriteBindings(const Context& ctx, const HeapPassBindings& b, uint32_t index, Args&&... args) noexcept;

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
    [[nodiscard]] auto AllocateStaticResourceSlot() noexcept -> std::expected<uint32_t, Error>;
    void               FreeStaticResourceSlot(uint32_t slot) noexcept;
    [[nodiscard]] auto AllocateStaticSamplerSlot() noexcept -> std::expected<uint32_t, Error>;
    void               FreeStaticSamplerSlot(uint32_t slot) noexcept;

    [[nodiscard]] auto AllocateDynamicResourceRangeSlot(uint32_t count) noexcept -> std::expected<uint32_t, Error>;
    [[nodiscard]] auto AllocateDynamicSamplerRangeSlot(uint32_t count) noexcept -> std::expected<uint32_t, Error>;

    DescriptorHeap<DescriptorHeapType::Resource> _resourceHeap;
    DescriptorHeap<DescriptorHeapType::Sampler>  _samplerHeap;

    uint32_t _staticResourceCount  = 0;
    uint32_t _dynamicResourceCount = 0;
    uint32_t _staticSamplerCount   = 0;
    uint32_t _dynamicSamplerCount  = 0;
    uint32_t _doubleBufferCount    = 2;
    uint32_t _currentFrameIndex    = 0;

    VkDeviceSize _maxPushDataSize = 0;

    SlotAllocator _staticResourceAlloc;
    SlotAllocator _staticSamplerAlloc;

    uint32_t _dynamicResourceAllocated = 0;
    uint32_t _dynamicSamplerAllocated  = 0;
};

} // namespace ZHLN::Vk
