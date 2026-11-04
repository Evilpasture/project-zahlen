#pragma once

// The two global shader-visible descriptor heaps of the SM 6.6 bindless
// model -- the D3D12 counterpart of the Vulkan descriptor-heap manager.
// There are no descriptor tables and no per-draw binding: resources are
// written into ResourceDescriptorHeap once at creation and shaders index
// the heap directly by slot (gpu_memory.slang). This class owns one
// ID3D12DescriptorHeap and its slot allocator (bump + free-list); the
// GlobalDescriptorHeaps pair below is the backend-wide instance shape.
//
// Slot indices are the logical slots of D3D::BufferAddress: slot 0 is a
// real, valid slot here; the +1 encoding that keeps raw zero null happens
// in BufferAddress::Encode(), never in this allocator.

#include "Com.hpp"
#include "D3D12Error.hpp"

#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <vector>

namespace ZHLN::D3D12 {

class DescriptorHeap {
  public:
    DescriptorHeap() noexcept = default;

    DescriptorHeap(const DescriptorHeap&)                    = delete;
    auto operator=(const DescriptorHeap&) -> DescriptorHeap& = delete;
    DescriptorHeap(DescriptorHeap&&) noexcept;
    auto operator=(DescriptorHeap&&) noexcept -> DescriptorHeap&;

    [[nodiscard]] static auto Create(
        ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, bool shaderVisible
    ) noexcept -> std::expected<DescriptorHeap, ErrorCode>;

    [[nodiscard]] auto Allocate() noexcept -> std::optional<uint32_t>;
    void               Free(uint32_t slot) noexcept;

    [[nodiscard]] auto CpuHandle(uint32_t slot) const noexcept -> D3D12_CPU_DESCRIPTOR_HANDLE;
    [[nodiscard]] auto GpuHandle(uint32_t slot) const noexcept -> D3D12_GPU_DESCRIPTOR_HANDLE;

    [[nodiscard]] auto Get() const noexcept -> ID3D12DescriptorHeap* {
        return _heap.Get();
    }
    [[nodiscard]] auto Capacity() const noexcept -> uint32_t {
        return _capacity;
    }
    [[nodiscard]] auto DescriptorSize() const noexcept -> uint32_t {
        return _descriptorSize;
    }
    explicit operator bool() const noexcept {
        return _heap != nullptr;
    }

  private:
    ComPtr<ID3D12DescriptorHeap> _heap;
    uint32_t                     _capacity       = 0;
    uint32_t                     _descriptorSize = 0;
    uint32_t                     _next           = 0;
    std::vector<uint32_t>        _freeList;
    mutable std::mutex           _mutex;
};

// The backend's heap pair: a large CBV/SRV/UAV heap (the
// ResourceDescriptorHeap shaders index) and the sampler heap, which D3D12
// caps architecturally at 2048 shader-visible descriptors.
struct GlobalDescriptorHeaps {
    static constexpr uint32_t kDefaultResourceCapacity = 1'000'000;
    static constexpr uint32_t kSamplerCapacity         = 2048;

    DescriptorHeap resources;
    DescriptorHeap samplers;

    [[nodiscard]] static auto Create(ID3D12Device* device, uint32_t resourceCapacity = kDefaultResourceCapacity) noexcept
        -> std::expected<GlobalDescriptorHeaps, ErrorCode>;
};

} // namespace ZHLN::D3D12
