// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/GeometryManager.cpp

#include "GeometryManager.hpp"

#include <cstring>

namespace ZHLN {

auto GeometryManager::CreateBuffer(size_t size, const void* data, Vk::BufferUsage usage) const
    -> std::expected<std::pair<Vk::Buffer, VkDeviceAddress>, ErrorCode> {
    // Buffers uploaded on the transfer queue get read (and sometimes written)
    // by the graphics AND compute families (cluster culling, particles,
    // skinning all dispatch on the compute queue). Buffers have no hardware
    // compression state to lose, so sharing them CONCURRENT across every
    // family that may touch them is free -- and it removes queue-family
    // ownership transfers from the upload path entirely. Deduplicate: on
    // unified hardware two or three of these indices are identical.
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

    return Vk::Buffer::Create(
               _allocator.Get(), size, usage | Vk::BufferUsage::TransferDst | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly, 0, sharingMode,
               {families, familyCount}
    )
        .transform([this, size, data](auto&& gpu_buf) -> auto {
            auto stagingAlloc = _transferRing.Allocate(size);

            if (data != nullptr) {
                std::memcpy(stagingAlloc.mappedData, data, size);
            } else {
                // Zero rather than leave uninitialised: the temporal passes read
                // a buffer on the frame it is created, before anything wrote it.
                std::memset(stagingAlloc.mappedData, 0, size);
            }

            // No release/acquire handoff: the buffer is CONCURRENT across the
            // families above. ExecuteImmediate's timeline-semaphore wait retires
            // the copy before this function returns, which orders it ahead of
            // every later queue submission.
            Vk::ExecuteImmediate<Vk::QueueType::Transfer>(_ctx, _transferCmdRing, _transferRing, [&](VkCommandBuffer cmd) -> void {
                Vk::CopyRingBuffer(cmd, stagingAlloc, gpu_buf, size);
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
    // No initial contents and no vertex count: a plain storage allocation.
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
    auto* nativeMesh = _buffers.Resolve(handle).value_or(nullptr);
    if (nativeMesh == nullptr) {
        return;
    }

    auto stagingAlloc = _transferRing.Allocate(size);
    std::memcpy(stagingAlloc.mappedData, data, size);

    Vk::ExecuteImmediate<Vk::QueueType::Transfer>(_ctx, _transferCmdRing, _transferRing, [&](VkCommandBuffer cmd) -> void {
        Vk::CopyRingBuffer(cmd, stagingAlloc, nativeMesh->buffer, size);
    });
}

void GeometryManager::Destroy(BufferHandle handle) {
    if (handle != BufferHandle::Invalid) {
        // Defer destruction for 2 frames so the GPU finishes reading from the
        // buffer. The pool releases the Vk::Buffer; the scoped guard routes it
        // into the deletion queue instead of destroying it inline.
        Vk::ScopedDeletionQueue guard(_deletionQueue);
        _buffers.Destroy(handle);
    }
}

} // namespace ZHLN
