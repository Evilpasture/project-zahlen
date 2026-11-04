// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Buffers.hpp"

namespace ZHLN::D3D12 {

auto Buffer::Create(ID3D12Device* device, DescriptorHeap& resourceHeap, const BufferDesc& desc) noexcept -> std::expected<Buffer, ErrorCode> {
    Buffer buffer;
    buffer._size = desc.size;

    const auto slot = resourceHeap.Allocate();
    if (!slot) {
        return std::unexpected(ErrorCode {D3D12Error::DescriptorSlotsExhausted});
    }
    buffer._slot = *slot;

    const bool isUpload = desc.heap == D3D12_HEAP_TYPE_UPLOAD;

    const D3D12_HEAP_PROPERTIES heapProps {
        .Type                 = desc.heap,
        .CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask     = 1,
        .VisibleNodeMask      = 1,
    };
    const D3D12_RESOURCE_DESC resourceDesc {
        .Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment        = 0,
        .Width            = desc.size,
        .Height           = 1,
        .DepthOrArraySize = 1,
        .MipLevels        = 1,
        .Format           = DXGI_FORMAT_UNKNOWN,
        .SampleDesc       = {.Count = 1, .Quality = 0},
        .Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags            = desc.usage == BufferUsage::ShaderReadWrite ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE,
    };
    // Buffers are created in COMMON (UPLOAD heaps require GENERIC_READ);
    // enhanced barriers promote as needed from there.
    const D3D12_RESOURCE_STATES initialState = isUpload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON;
    if (const HRESULT hr = device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState, nullptr, __uuidof(ID3D12Resource),
            reinterpret_cast<void**>(buffer._resource.AddressOf())
        );
        FAILED(hr)) {
        resourceHeap.Free(buffer._slot);
        return std::unexpected(ToError(hr));
    }

    // RAW byte-address view over the whole buffer: NumElements counts
    // uint32 lanes, matching ByteAddressBuffer addressing.
    const uint32_t numElements = static_cast<uint32_t>(desc.size / sizeof(uint32_t));
    if (desc.usage == BufferUsage::ShaderReadWrite) {
        const D3D12_UNORDERED_ACCESS_VIEW_DESC uav {
            .Format        = DXGI_FORMAT_R32_TYPELESS,
            .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
            .Buffer =
                {
                    .FirstElement         = 0,
                    .NumElements          = numElements,
                    .StructureByteStride  = 0,
                    .CounterOffsetInBytes = 0,
                    .Flags                = D3D12_BUFFER_UAV_FLAG_RAW,
                },
        };
        device->CreateUnorderedAccessView(buffer._resource.Get(), nullptr, &uav, resourceHeap.CpuHandle(buffer._slot));
    } else {
        const D3D12_SHADER_RESOURCE_VIEW_DESC srv {
            .Format                  = DXGI_FORMAT_R32_TYPELESS,
            .ViewDimension           = D3D12_SRV_DIMENSION_BUFFER,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Buffer =
                {
                    .FirstElement        = 0,
                    .NumElements         = numElements,
                    .StructureByteStride = 0,
                    .Flags               = D3D12_BUFFER_SRV_FLAG_RAW,
                },
        };
        device->CreateShaderResourceView(buffer._resource.Get(), &srv, resourceHeap.CpuHandle(buffer._slot));
    }
    return buffer;
}

auto Buffer::Map() noexcept -> void* {
    void* mapped = nullptr;
    // Buffers are written whole; the empty read range says so (no
    // CPU-read stall on a write-only mapping).
    const D3D12_RANGE noRead {0, 0};
    if (FAILED(_resource.Get()->Map(0, &noRead, &mapped))) {
        return nullptr;
    }
    return mapped;
}

void Buffer::Unmap() noexcept {
    const D3D12_RANGE noWrite {0, 0};
    _resource.Get()->Unmap(0, &noWrite);
}

} // namespace ZHLN::D3D12
