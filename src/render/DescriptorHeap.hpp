// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/DescriptorHeap.hpp

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

/*
 * EXPERIMENTAL: Might not be available in MacOS.
 * Might be available using KosmicKrisp instead of MoltenVK.
 */

enum class DescriptorHeapType : uint8_t {
    Resource, // Storage Buffers, Uniform Buffers, Sampled Images, Storage Images, AS
    Sampler   // Samplers only
};

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

class DescriptorHeap {
  public:
    DescriptorHeap() = default;
    ~DescriptorHeap() noexcept;

    DescriptorHeap(const DescriptorHeap&)                    = delete;
    auto operator=(const DescriptorHeap&) -> DescriptorHeap& = delete;

    DescriptorHeap(DescriptorHeap&& other) noexcept;
    auto operator=(DescriptorHeap&& other) noexcept -> DescriptorHeap&;

    [[nodiscard]] auto Init(const Context& ctx, Allocator& allocator, DescriptorHeapType type, uint32_t capacity) noexcept -> bool;
    void               Cleanup() noexcept;

    void Bind(VkCommandBuffer cmd) const noexcept;

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
    [[nodiscard]] auto GetType() const noexcept -> DescriptorHeapType {
        return _type;
    }
    [[nodiscard]] auto GetMappedPtr() const noexcept -> void* {
        return _mappedPtr;
    }
    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _buffer.Valid();
    }
    explicit operator bool() const noexcept {
        return Valid();
    }

    [[nodiscard]] auto GetWriteResourceDescriptorsFn() const noexcept -> PFN_vkWriteResourceDescriptorsEXT {
        return _vkWriteResourceDescriptorsEXT;
    }
    [[nodiscard]] auto GetWriteSamplerDescriptorsFn() const noexcept -> PFN_vkWriteSamplerDescriptorsEXT {
        return _vkWriteSamplerDescriptorsEXT;
    }

  private:
    VkDevice           _device   = VK_NULL_HANDLE;
    DescriptorHeapType _type     = DescriptorHeapType::Resource;
    uint32_t           _capacity = 0;
    VkDeviceSize       _stride   = 0;

    Buffer               _buffer;
    Buffer::MappedRegion _mappedRegion;
    void*                _mappedPtr     = nullptr;
    VkDeviceAddress      _deviceAddress = 0;

    PFN_vkCmdBindSamplerHeapEXT       _vkCmdBindSamplerHeapEXT       = nullptr;
    PFN_vkCmdBindResourceHeapEXT      _vkCmdBindResourceHeapEXT      = nullptr;
    PFN_vkWriteSamplerDescriptorsEXT  _vkWriteSamplerDescriptorsEXT  = nullptr;
    PFN_vkWriteResourceDescriptorsEXT _vkWriteResourceDescriptorsEXT = nullptr;
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

    void
        AddImage(uint32_t slot, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout, VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) noexcept;

    void AddBuffer(uint32_t slot, VkDeviceAddress address, VkDeviceSize size, VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) noexcept;

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

    void AddSampler(uint32_t slot, const VkSamplerCreateInfo& createInfo) noexcept;

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

    void               Init(uint32_t capacity) noexcept;
    [[nodiscard]] auto Allocate() noexcept -> uint32_t;
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
    ) noexcept -> bool;

    void BeginFrame(uint32_t frameIndex) noexcept;

    // --- Static Allocation ---

    [[nodiscard]] auto AllocateStaticResourceSlot() noexcept -> uint32_t;
    void               FreeStaticResourceSlot(uint32_t slot) noexcept;
    [[nodiscard]] auto AllocateStaticSamplerSlot() noexcept -> uint32_t;
    void               FreeStaticSamplerSlot(uint32_t slot) noexcept;

    // --- Dynamic/Transient Allocation ---

    [[nodiscard]] auto AllocateDynamicResourceRange(uint32_t count) noexcept -> uint32_t;
    [[nodiscard]] auto AllocateDynamicSamplerRange(uint32_t count) noexcept -> uint32_t;

    // --- Updates ---

    void FlushResourceBatch(ResourceWriteBatch& batch) noexcept;
    void FlushSamplerBatch(SamplerWriteBatch& batch) noexcept;

    // --- Command Binding ---

    void BindHeaps(VkCommandBuffer cmd) const noexcept;

  private:
    DescriptorHeap _resourceHeap;
    DescriptorHeap _samplerHeap;

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
