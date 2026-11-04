#pragma once

// Buffer creation for the SM 6.6 bindless model: a buffer is a committed
// ID3D12Resource plus one descriptor written into the global resource
// heap at creation; the returned BufferAddress (slot + byte offset, via
// D3D::BufferAddress) is what crosses to shaders as DeviceAddress<T>.
//
// View kind is fixed at creation and never reinterpreted at access time:
// ShaderReadWrite mints a UAV, ShaderRead an SRV (the todo's descriptor
// view-kind rule). Both are RAW (R32_TYPELESS) byte-address views because
// gpu_memory.slang accesses buffers as ByteAddressBuffer /
// RWByteAddressBuffer and does its own typed Load<T>/Store<T>.

#include "Com.hpp"
#include "D3D12Error.hpp"
#include "DescriptorHeap.hpp"

#include <d3d12/memory/BufferAddress.hpp>

#include <cstdint>
#include <expected>

namespace ZHLN::D3D12 {

enum class BufferUsage : uint8_t {
    ShaderRead,      // SRV: load() only
    ShaderReadWrite, // UAV: load()/store()/atomicAdd()
};

struct BufferDesc {
    uint64_t    size  = 0;
    BufferUsage usage = BufferUsage::ShaderReadWrite;
    // DEFAULT for GPU-resident buffers; UPLOAD for CPU-written staging
    // (which then also permits Map()). Readback is a later addition.
    D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT;
};

class Buffer {
  public:
    Buffer() noexcept = default;

    Buffer(const Buffer&)                    = delete;
    auto operator=(const Buffer&) -> Buffer& = delete;
    Buffer(Buffer&&) noexcept                = default;
    auto operator=(Buffer&&) noexcept -> Buffer& = default;

    // On success the descriptor heap owns the view; this object owns the
    // resource and remembers its slot. Destroying the Buffer releases the
    // resource but NOT the slot semantics: call FreeSlot() (or let the
    // heap die with the backend) -- slot return is explicit because a
    // descriptor may still be referenced by in-flight frames.
    [[nodiscard]] static auto Create(ID3D12Device* device, DescriptorHeap& resourceHeap, const BufferDesc& desc) noexcept
        -> std::expected<Buffer, ErrorCode>;

    void FreeSlot(DescriptorHeap& resourceHeap) noexcept {
        if (_slot != D3D::BufferAddress<>::InvalidSlot) {
            resourceHeap.Free(_slot);
            _slot = D3D::BufferAddress<>::InvalidSlot;
        }
    }

    template <typename T = void>
    [[nodiscard]] auto Address() const noexcept -> D3D::BufferAddress<T> {
        return D3D::BufferAddress<T>(_slot, 0);
    }
    // The 64-bit wire word shaders receive (one-based slot | offset).
    [[nodiscard]] auto EncodedAddress() const noexcept -> uint64_t {
        return Address<>().Encode();
    }

    [[nodiscard]] auto Get() const noexcept -> ID3D12Resource* {
        return _resource.Get();
    }
    [[nodiscard]] auto Size() const noexcept -> uint64_t {
        return _size;
    }
    [[nodiscard]] auto Slot() const noexcept -> uint32_t {
        return _slot;
    }
    explicit operator bool() const noexcept {
        return _resource != nullptr;
    }

    // CPU access for UPLOAD-heap buffers; nullptr for DEFAULT heap.
    [[nodiscard]] auto Map() noexcept -> void*;
    void               Unmap() noexcept;

  private:
    ComPtr<ID3D12Resource> _resource;
    uint64_t               _size = 0;
    uint32_t               _slot = D3D::BufferAddress<>::InvalidSlot;
};

} // namespace ZHLN::D3D12
