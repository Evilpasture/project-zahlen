// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "GeometryManager.hpp"

#include <Zahlen/Vertex.hpp>
#include <cstring>

namespace ZHLN {

auto GeometryManager::CreateBuffer(size_t size, const void* data, Vk::BufferUsage usage) const
    -> std::expected<std::pair<Vk::Buffer, VkDeviceAddress>, ErrorCode> {
    const auto&    familyInfo    = _ctx.PhysicalInfo();
    const uint32_t candidates[3] = {familyInfo.graphics_family, familyInfo.transfer_family, familyInfo.compute_family};
    uint32_t       families[3];
    uint32_t       familyCount = 0;
    for (const uint32_t candidate: candidates) {
        bool seen = false;
        for (uint32_t i = 0; i < familyCount; ++i) {
            seen = seen || families[i] == candidate;
        }
        if (!seen) {
            families[familyCount++] = candidate;
        }
    }
    const VkSharingMode sharingMode = (familyCount > 1) ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;

    const Vk::BufferUsage rtBit =
        _ctx.RayTracingSupported() ? Vk::BufferUsage::AccelerationStructureBuildInput : Vk::BufferUsage::None;

    return Vk::Buffer::Create(
               _allocator.Get(), size, usage | rtBit | Vk::BufferUsage::TransferDst | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly, 0,
               sharingMode, {families, familyCount}
    )
        .transform([this, size, data](auto&& gpu_buf) -> auto {
            auto stagingAlloc = _transferRing.Allocate(size);

            if (data != nullptr) {
                std::memcpy(stagingAlloc.mappedData, data, size);
            } else {
                std::memset(stagingAlloc.mappedData, 0, size);
            }

            Vk::ExecuteImmediate<Vk::QueueType::Transfer>(_ctx, _transferCmdRing, _transferRing, [&](VkCommandBuffer cmd) -> void {
                Vk::CopyRingBuffer(cmd, stagingAlloc, gpu_buf);
            });

            VkDeviceAddress address = Vk::GetBufferAddress(_ctx.Device(), gpu_buf.Handle());
            return std::make_pair(std::forward<decltype(gpu_buf)>(gpu_buf), address);
        });
}

auto GeometryManager::Adopt(Vk::Buffer&& buffer, uint32_t vertexCount, VkDeviceAddress address) -> BufferHandle {
    return _buffers.Create(std::move(buffer), vertexCount, address);
}

auto GeometryManager::CreateVertexBuffer(const void* data, size_t size, uint32_t stride, Vk::BufferUsage usage) -> BufferHandle {
    const uint32_t safeStride = (stride > 0) ? stride : 1u;
    return CreateBuffer(size, data, usage)
        .transform([this, size, safeStride](auto&& pair) -> BufferHandle {
            return Adopt(std::move(pair.first), static_cast<uint32_t>(size / safeStride), pair.second);
        })
        .value_or(BufferHandle::Invalid);
}

auto GeometryManager::CreateIndexBuffer(const void* data, size_t size, Vk::BufferUsage usage) -> BufferHandle {
    return CreateBuffer(size, data, usage)
        .transform([this, size](auto&& pair) -> BufferHandle {
            return Adopt(std::move(pair.first), static_cast<uint32_t>(size / sizeof(uint32_t)), pair.second);
        })
        .value_or(BufferHandle::Invalid);
}

auto GeometryManager::CreateStorageBuffer(size_t size, Vk::BufferUsage usage) -> BufferHandle {
    return CreateBuffer(size, nullptr, usage)
        .transform([this](auto&& pair) -> BufferHandle { return Adopt(std::move(pair.first), 0, pair.second); })
        .value_or(BufferHandle::Invalid);
}

auto GeometryManager::CreateStorageBuffer(const void* data, size_t size, uint32_t stride, Vk::BufferUsage usage) -> BufferHandle {
    const uint32_t safeStride = (stride > 0) ? stride : 1u;
    return CreateBuffer(size, data, usage)
        .transform([this, size, safeStride](auto&& pair) -> BufferHandle {
            return Adopt(std::move(pair.first), static_cast<uint32_t>(size / safeStride), pair.second);
        })
        .value_or(BufferHandle::Invalid);
}

void GeometryManager::Update(BufferHandle handle, const void* data, size_t size) noexcept {
    if (handle == BufferHandle::Invalid || data == nullptr || size == 0) {
        return;
    }
    auto* nativeMesh = _buffers.Resolve(handle);
    if (nativeMesh == nullptr) {
        return;
    }

    auto stagingAlloc = _transferRing.Allocate(size);
    std::memcpy(stagingAlloc.mappedData, data, size);

    Vk::ExecuteImmediate<Vk::QueueType::Transfer>(_ctx, _transferCmdRing, _transferRing, [&](VkCommandBuffer cmd) -> void {
        Vk::CopyRingBuffer(cmd, stagingAlloc, nativeMesh->buffer);
    });
}

auto GeometryManager::CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle {
    const size_t size = (static_cast<size_t>(vertexCount) * sizeof(VertexPosition)) + (static_cast<size_t>(vertexCount) * sizeof(VertexAttributes));

    Vk::BufferUsage usage = Vk::BufferUsage::Vertex | Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress;
    if (_ctx.RayTracingSupported()) {
        usage |= Vk::BufferUsage::AccelerationStructureBuildInput;
    }

    return Vk::Buffer::Create(_allocator.Get(), size, usage, Vk::MemoryUsage::GPUOnly)
        .transform([this, vertexCount](auto&& gpu_buf) -> BufferHandle {
            const VkDeviceAddress address = Vk::GetBufferAddress(_ctx.Device(), gpu_buf.Handle());
            return Adopt(std::forward<decltype(gpu_buf)>(gpu_buf), vertexCount, address);
        })
        .value_or(BufferHandle::Invalid);
}

void GeometryManager::Destroy(BufferHandle handle) {
    if (handle != BufferHandle::Invalid) {
        Vk::ScopedDeletionQueue guard(_deletionQueue);
        _buffers.Destroy(handle);
    }
}

}
