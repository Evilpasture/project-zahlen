// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/DescriptorHeap.hpp

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

// Forward declarations to break inline dependency loops
class ResourceWriteBatch;
class SamplerWriteBatch;
class HeapManager;

enum class DescriptorHeapType : uint8_t {
    Resource, // Storage Buffers, Uniform Buffers, Sampled Images, Storage Images, AS
    Sampler   // Samplers only
};

enum class DescriptorHeapError : uint8_t {
    ResourceSlotsExhausted,
    SamplerSlotsExhausted,
    DynamicResourceOverflow,
    DynamicSamplerOverflow,
    FunctionLoaderFailed,
    AllocationFailed,
    MappingFailed,
    DeviceAddressFailed
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

// ============================================================================
// Function Pointer Signatures
// ============================================================================

using PFN_vkCmdBindSamplerHeapEXT  = void(VKAPI_PTR*)(VkCommandBuffer commandBuffer, const VkBindHeapInfoEXT* pBindInfo);
using PFN_vkCmdBindResourceHeapEXT = void(VKAPI_PTR*)(VkCommandBuffer commandBuffer, const VkBindHeapInfoEXT* pBindInfo);
using PFN_vkWriteSamplerDescriptorsEXT =
    VkResult(VKAPI_PTR*)(VkDevice device, uint32_t samplerCount, const VkSamplerCreateInfo* pSamplers, const VkHostAddressRangeEXT* pDescriptors);
using PFN_vkWriteResourceDescriptorsEXT =
    VkResult(VKAPI_PTR*)(VkDevice device, uint32_t resourceCount, const VkResourceDescriptorInfoEXT* pResources, const VkHostAddressRangeEXT* pDescriptors);

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

    [[nodiscard]] auto Init(const Context& ctx, Allocator& allocator, uint32_t capacity) noexcept -> std::expected<void, DescriptorHeapError>;
    void               Cleanup() noexcept;

    void Bind(VkCommandBuffer cmd) const noexcept;

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

  private:
    friend class HeapManager;

    [[nodiscard]] auto GetDevice() const noexcept -> VkDevice {
        return _device;
    }
    [[nodiscard]] auto GetBuffer() const noexcept -> VkBuffer {
        return _buffer.Handle();
    }
    [[nodiscard]] auto GetDeviceAddress() const noexcept -> VkDeviceAddress {
        return _deviceAddress;
    }
    [[nodiscard]] auto GetStride() const noexcept -> VkDeviceSize {
        return _stride;
    }
    [[nodiscard]] auto GetCapacity() const noexcept -> uint32_t {
        return _capacity;
    }
    [[nodiscard]] auto GetMappedPtr() const noexcept -> void* {
        return _mappedPtr;
    }

    VkDevice     _device   = VK_NULL_HANDLE;
    uint32_t     _capacity = 0;
    VkDeviceSize _stride   = 0;

    Buffer               _buffer;
    Buffer::MappedRegion _mappedRegion;
    void*                _mappedPtr     = nullptr;
    VkDeviceAddress      _deviceAddress = 0;

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

    void               Init(uint32_t capacity, DescriptorHeapError errorOnExhaustion) noexcept;
    [[nodiscard]] auto Allocate() noexcept -> std::expected<uint32_t, DescriptorHeapError>;
    void               Free(uint32_t slot) noexcept;
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

    [[nodiscard]] auto Init(
        const Context& ctx,
        Allocator&     allocator,
        uint32_t       staticResourceCount,
        uint32_t       dynamicResourceCount,
        uint32_t       staticSamplerCount,
        uint32_t       dynamicSamplerCount,
        uint32_t       doubleBufferCount = 2
    ) noexcept -> std::expected<void, DescriptorHeapError>;

    void BeginFrame(uint32_t frameIndex) noexcept;

    // --- Type-Safe Static Resource Allocation ---
    template <VkDescriptorType Type>
        requires ValidResourceDescriptorType<Type>
    [[nodiscard]] auto AllocateStaticResource() noexcept -> std::expected<HeapHandle<DescriptorHeapType::Resource, Type>, DescriptorHeapError> {
        return AllocateStaticResourceSlot().transform([](uint32_t idx) { return HeapHandle<DescriptorHeapType::Resource, Type> {idx}; });
    }

    [[nodiscard]] auto AllocateStaticSampler() noexcept -> std::expected<SamplerHandle, DescriptorHeapError> {
        return AllocateStaticSamplerSlot().transform([](uint32_t idx) { return SamplerHandle {idx}; });
    }

    // --- Type-Safe Dynamic Range Allocation ---
    template <VkDescriptorType Type>
        requires ValidResourceDescriptorType<Type>
    [[nodiscard]] auto
        AllocateDynamicResourceRange(uint32_t count) noexcept -> std::expected<HeapHandle<DescriptorHeapType::Resource, Type>, DescriptorHeapError> {
        return AllocateDynamicResourceRangeSlot(count).transform([](uint32_t idx) { return HeapHandle<DescriptorHeapType::Resource, Type> {idx}; });
    }

    [[nodiscard]] auto AllocateDynamicSamplerRange(uint32_t count) noexcept -> std::expected<SamplerHandle, DescriptorHeapError> {
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

    // --- Updates ---
    void FlushResourceBatch(ResourceWriteBatch& batch) noexcept;
    void FlushSamplerBatch(SamplerWriteBatch& batch) noexcept;

    // --- Command Binding ---
    void BindHeaps(VkCommandBuffer cmd) const noexcept;

  private:
    [[nodiscard]] auto AllocateStaticResourceSlot() noexcept -> std::expected<uint32_t, DescriptorHeapError>;
    void               FreeStaticResourceSlot(uint32_t slot) noexcept;
    [[nodiscard]] auto AllocateStaticSamplerSlot() noexcept -> std::expected<uint32_t, DescriptorHeapError>;
    void               FreeStaticSamplerSlot(uint32_t slot) noexcept;

    [[nodiscard]] auto AllocateDynamicResourceRangeSlot(uint32_t count) noexcept -> std::expected<uint32_t, DescriptorHeapError>;
    [[nodiscard]] auto AllocateDynamicSamplerRangeSlot(uint32_t count) noexcept -> std::expected<uint32_t, DescriptorHeapError>;

    DescriptorHeap<DescriptorHeapType::Resource> _resourceHeap;
    DescriptorHeap<DescriptorHeapType::Sampler>  _samplerHeap;

    uint32_t _staticResourceCount  = 0;
    uint32_t _dynamicResourceCount = 0;
    uint32_t _staticSamplerCount   = 0;
    uint32_t _dynamicSamplerCount  = 0;
    uint32_t _doubleBufferCount    = 2;
    uint32_t _currentFrameIndex    = 0;

    SlotAllocator _staticResourceAlloc;
    SlotAllocator _staticSamplerAlloc;

    uint32_t _dynamicResourceAllocated = 0;
    uint32_t _dynamicSamplerAllocated  = 0;
};

} // namespace ZHLN::Vk
