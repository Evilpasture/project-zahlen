// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include "memory/Allocator.hpp"

#include <Zahlen/Threading/Mutex.hpp>
#include <utility>

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../VkError.hpp"
namespace ZHLN::Vk {

class ResourceWriteBatch;
class SamplerWriteBatch;
class HeapManager;
struct SamplerConfig;
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

    [[nodiscard]] auto Init(const Context& ctx, Allocator& allocator, uint32_t capacity) noexcept -> std::expected<void, Vk::Error>;
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
    // The slot ids live alongside their payloads, not in a parallel vector.
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

    void AddSampler(SamplerHandle handle, const SamplerConfig& config) noexcept;

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

    void               Init(uint32_t capacity, Vk::Error errorOnExhaustion) noexcept;
    [[nodiscard]] auto Allocate() noexcept -> std::expected<uint32_t, Vk::Error>;
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
    ) noexcept -> std::expected<void, Vk::Error>;

    void BeginFrame(uint32_t frameIndex) noexcept;

    void BeginImmediate() noexcept;

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _resourceHeap.Valid() && _samplerHeap.Valid();
    }

    [[nodiscard]] auto ReserveOffsetAddressedResourceRegion(uint32_t count) noexcept -> std::expected<uint32_t, Vk::Error>;
    [[nodiscard]] auto ReserveOffsetAddressedSamplerRegion(uint32_t count) noexcept -> std::expected<uint32_t, Vk::Error>;

    template <VkDescriptorType Type>
        requires ValidResourceDescriptorType<Type>
    [[nodiscard]] auto AllocateStaticResource() noexcept -> std::expected<HeapHandle<DescriptorHeapType::Resources, Type>, Vk::Error> {
        return AllocateStaticResourceSlot().transform([](uint32_t idx) { return HeapHandle<DescriptorHeapType::Resources, Type> {idx}; });
    }

    [[nodiscard]] auto AllocateStaticSampler() noexcept -> std::expected<SamplerHandle, Vk::Error> {
        return AllocateStaticSamplerSlot().transform([](uint32_t idx) { return SamplerHandle {idx}; });
    }

    [[nodiscard]] auto AllocateTransientResourceRange(uint32_t count, HeapLifecycle lifecycle) noexcept
        -> std::expected<uint32_t, Vk::Error>;

    template <VkDescriptorType Type>
    void FreeStaticResource(HeapHandle<DescriptorHeapType::Resources, Type> handle) noexcept {
        FreeStaticResourceSlot(handle.index);
    }

    void FreeStaticSampler(SamplerHandle handle) noexcept {
        FreeStaticSamplerSlot(handle.index);
    }

    void WriteImage(TextureHandle handle, const ImageView& view, VkImageLayout layout) noexcept;
    template <typename Resource>
        requires requires(const Resource& resource) { resource.view.Valid(); }
    void WriteImage(TextureHandle handle, const Resource& resource, VkImageLayout layout) noexcept {
        WriteImage(handle, resource.view, layout);
    }

    void WriteStorageImage(StorageImageHandle handle, const ImageView& view, VkImageLayout layout) noexcept;
    template <typename Resource>
        requires requires(const Resource& resource) { resource.view.Valid(); }
    void WriteStorageImage(StorageImageHandle handle, const Resource& resource, VkImageLayout layout) noexcept {
        WriteStorageImage(handle, resource.view, layout);
    }
    void WriteBuffer(StorageBufferHandle handle, BufferSlice slice) noexcept;
    void WriteBuffer(UniformBufferHandle handle, BufferSlice slice) noexcept;
    void WriteAccelerationStructure(AccelerationStructureHandle handle, VkDeviceAddress address) noexcept;
    void WriteSampler(SamplerHandle handle, const SamplerConfig& config) noexcept;

    template <typename Declared, typename... Slots>
    [[nodiscard]] auto WriteHeapParameters(const Context& ctx, const HeapPassBindings& b, const Slots&... slots) noexcept -> HeapBlockBase;

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
    [[nodiscard]] auto AllocateStaticResourceSlot() noexcept -> std::expected<uint32_t, Vk::Error>;
    void               FreeStaticResourceSlot(uint32_t slot) noexcept;
    [[nodiscard]] auto AllocateStaticSamplerSlot() noexcept -> std::expected<uint32_t, Vk::Error>;
    void               FreeStaticSamplerSlot(uint32_t slot) noexcept;

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

}
