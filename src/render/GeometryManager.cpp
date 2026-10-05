// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GeometryManager.hpp"
#include <Zahlen/Vertex.hpp>
#include <cstring>

namespace ZHLN {

auto GeometryManager::CreateBuffer(size_t size, const void* data, Vk::BufferUsage usage) const
    -> std::expected<std::pair<Vk::Buffer, VkDeviceAddress>, ErrorCode> {
    const auto&             familyInfo = _ctx.PhysicalInfo();
    const std::array        candidates = {familyInfo.graphics_family, familyInfo.transfer_family, familyInfo.compute_family};
    std::array<uint32_t, 3> families {};
    uint32_t                familyCount = 0;
    for (const uint32_t candidate: candidates) {
        bool seen = false;
        for (uint32_t i = 0; i < familyCount; ++i) {
            seen = seen || families[i] == candidate;
        }
        if (!seen) {
            families[familyCount++] = candidate;
        }
    }
    const auto sharingMode = (familyCount > 1) ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;

    const auto rtBit = _ctx.RayTracingSupported() ? Vk::BufferUsage::AccelerationStructureBuildInput : Vk::BufferUsage::None;

    return Vk::Buffer::Create(
               _allocator, size, usage | rtBit | Vk::BufferUsage::TransferDst | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly, 0, sharingMode,
               {families.data(), familyCount}
    )
        .and_then([this, size, data](Vk::Buffer gpu_buf) -> std::expected<std::pair<Vk::Buffer, VkDeviceAddress>, ErrorCode> {
            defer _([&] { _allocator.DestroyBuffer(gpu_buf); });
            auto  stagingAlloc = _transferRing.Allocate(size);
            if (stagingAlloc.mappedData == nullptr) {
                return std::unexpected(Vk::StagingError::MemoryMappingFailed);
            }

            if (data != nullptr) {
                std::memcpy(stagingAlloc.mappedData, data, size);
            } else {
                std::memset(stagingAlloc.mappedData, 0, size);
            }

            Vk::ExecuteImmediate<Vk::QueueType::Transfer>(_ctx, _transferCmdRing, _transferRing, [&](VkCommandBuffer cmd) -> void {
                Vk::CopyRingBuffer(cmd, stagingAlloc, gpu_buf);
            });

            VkDeviceAddress address = Vk::GetBufferAddress(_ctx.Device(), gpu_buf.Handle());
            return std::make_pair(std::move(gpu_buf), address);
        });
}

auto GeometryManager::Adopt(Vk::Buffer&& buffer, uint32_t vertexCount, VkDeviceAddress address) -> BufferHandle {
    const BufferHandle handle = _buffers.Create(std::move(buffer), vertexCount, address);
    if (handle == BufferHandle::Invalid) {
        _allocator.DestroyBuffer(buffer); // Pool full: Create did not take the rvalue.
    }
    return handle;
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
    if (stagingAlloc.mappedData == nullptr) {
        return;
    }
    std::memcpy(stagingAlloc.mappedData, data, size);

    Vk::ExecuteImmediate<Vk::QueueType::Transfer>(_ctx, _transferCmdRing, _transferRing, [&](VkCommandBuffer cmd) -> void {
        Vk::CopyRingBuffer(cmd, stagingAlloc, nativeMesh->buffer);
    });
}

auto GeometryManager::CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle {
    // Deform only positions and the tangent frame. UV/color data stays in
    // the immutable VertexSurface buffer owned by the source mesh.
    const size_t size = (static_cast<size_t>(vertexCount) * sizeof(VertexPosition)) + (static_cast<size_t>(vertexCount) * sizeof(VertexTangentFrame));

    Vk::BufferUsage usage = Vk::BufferUsage::Vertex | Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress;
    if (_ctx.RayTracingSupported()) {
        usage |= Vk::BufferUsage::AccelerationStructureBuildInput;
    }

    return Vk::Buffer::Create(_allocator, size, usage, Vk::MemoryUsage::GPUOnly)
        .transform([this, vertexCount](auto&& gpu_buf) -> BufferHandle {
            const VkDeviceAddress address = Vk::GetBufferAddress(_ctx.Device(), gpu_buf.Handle());
            return Adopt(std::forward<decltype(gpu_buf)>(gpu_buf), vertexCount, address);
        })
        .value_or(BufferHandle::Invalid);
}

void GeometryManager::Retire(NativeMesh& mesh) noexcept {
    // The AS is a GPU object too. Retire it ahead of its backing allocation,
    // then the mesh allocation, in the same frame-delayed batch.
    _deletionQueue.EnqueueAccelerationStructure(_ctx.Device(), std::move(mesh.blas));
    _deletionQueue.Enqueue(std::move(mesh.blasBuffer));
    _deletionQueue.Enqueue(std::move(mesh.buffer));
}

void GeometryManager::Destroy(BufferHandle handle) {
    if (auto* mesh = _buffers.Resolve(handle)) {
        Retire(*mesh);
        _buffers.Destroy(handle);
    }
}

void GeometryManager::RetireAll() noexcept {
    _buffers.ForEachLive([this](NativeMesh& mesh) { Retire(mesh); });
}

} // namespace ZHLN
