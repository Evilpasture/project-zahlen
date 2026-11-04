// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DescriptorHeap.hpp"

#include <utility>

namespace ZHLN::D3D12 {

DescriptorHeap::DescriptorHeap(DescriptorHeap&& other) noexcept
    : _heap(std::move(other._heap)), _capacity(other._capacity), _descriptorSize(other._descriptorSize),
      _next(other._next), _freeList(std::move(other._freeList)) {
}

auto DescriptorHeap::operator=(DescriptorHeap&& other) noexcept -> DescriptorHeap& {
    if (this != &other) {
        _heap           = std::move(other._heap);
        _capacity       = other._capacity;
        _descriptorSize = other._descriptorSize;
        _next           = other._next;
        _freeList       = std::move(other._freeList);
    }
    return *this;
}

auto DescriptorHeap::Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, bool shaderVisible) noexcept
    -> std::expected<DescriptorHeap, ErrorCode> {
    DescriptorHeap heap;
    heap._capacity       = capacity;
    heap._descriptorSize = device->GetDescriptorHandleIncrementSize(type);

    const D3D12_DESCRIPTOR_HEAP_DESC desc {
        .Type           = type,
        .NumDescriptors = capacity,
        .Flags          = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask       = 0,
    };
    if (const HRESULT hr = device->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(heap._heap.AddressOf()));
        FAILED(hr)) {
        return std::unexpected(ToError(hr));
    }
    return heap;
}

auto DescriptorHeap::Allocate() noexcept -> std::optional<uint32_t> {
    const std::lock_guard lock(_mutex);
    if (!_freeList.empty()) {
        const uint32_t slot = _freeList.back();
        _freeList.pop_back();
        return slot;
    }
    if (_next < _capacity) {
        return _next++;
    }
    return std::nullopt;
}

void DescriptorHeap::Free(uint32_t slot) noexcept {
    const std::lock_guard lock(_mutex);
    _freeList.push_back(slot);
}

auto DescriptorHeap::CpuHandle(uint32_t slot) const noexcept -> D3D12_CPU_DESCRIPTOR_HANDLE {
    // The two header flavors disagree on this getter's shape: the real
    // SDK exposes the pointer-form vtable slot (the value-returning
    // wrapper is absent under the GNU ABI), the WSL stubs the value form.
    // Take whichever the platform offers.
#if defined(_WIN32)
    D3D12_CPU_DESCRIPTOR_HANDLE handle {};
    _heap.Get()->GetCPUDescriptorHandleForHeapStart(&handle);
#else
    D3D12_CPU_DESCRIPTOR_HANDLE handle = _heap.Get()->GetCPUDescriptorHandleForHeapStart();
#endif
    handle.ptr += static_cast<size_t>(slot) * _descriptorSize;
    return handle;
}

auto DescriptorHeap::GpuHandle(uint32_t slot) const noexcept -> D3D12_GPU_DESCRIPTOR_HANDLE {
#if defined(_WIN32)
    D3D12_GPU_DESCRIPTOR_HANDLE handle {};
    _heap.Get()->GetGPUDescriptorHandleForHeapStart(&handle);
#else
    D3D12_GPU_DESCRIPTOR_HANDLE handle = _heap.Get()->GetGPUDescriptorHandleForHeapStart();
#endif
    handle.ptr += static_cast<uint64_t>(slot) * _descriptorSize;
    return handle;
}

auto GlobalDescriptorHeaps::Create(ID3D12Device* device, uint32_t resourceCapacity) noexcept
    -> std::expected<GlobalDescriptorHeaps, ErrorCode> {
    auto resources = DescriptorHeap::Create(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, resourceCapacity, true);
    if (!resources) {
        return std::unexpected(resources.error());
    }
    auto samplers = DescriptorHeap::Create(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerCapacity, true);
    if (!samplers) {
        return std::unexpected(samplers.error());
    }
    GlobalDescriptorHeaps heaps;
    heaps.resources = std::move(*resources);
    heaps.samplers  = std::move(*samplers);
    return heaps;
}

} // namespace ZHLN::D3D12
