// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "GeometryManager.hpp"

#include <Zahlen/Core/Ranges.hpp>
#include <Zahlen/Vertex.hpp>
#include <array>
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
        Vk::CopyRingBuffer(cmd, stagingAlloc, nativeMesh->buffer, size);
    });
}

auto GeometryManager::GetOrCreateParticleBuffer(uint64_t cacheKey, uint64_t packedOwner, size_t byteSize, Vk::BufferUsage usage) -> BufferHandle {
    const auto* existing = _particleBuffers.Find(cacheKey);
    if (existing != nullptr && existing->second != BufferHandle::Invalid) {
        return existing->second;
    }

    BufferHandle handle = CreateStorageBuffer(byteSize, usage);
    if (handle != BufferHandle::Invalid) {
        _particleBuffers.Insert(cacheKey, {packedOwner, handle});
    }
    return handle;
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

auto GeometryManager::GetOrCreateSkinnedScratchBuffer(uint64_t entityKey, uint32_t vertexCount) -> BufferHandle {
    const BufferHandle* existing = _skinnedScratch.Find(entityKey);
    if (existing != nullptr && *existing != BufferHandle::Invalid) {
        return *existing;
    }

    const BufferHandle handle = CreateSkinnedScratchBuffer(vertexCount);
    if (handle != BufferHandle::Invalid) {
        _skinnedScratch.Insert(entityKey, handle);
    }
    return handle;
}

void GeometryManager::ReleaseSkinnedScratchBuffers() {
    _skinnedScratch.ForEach([this](uint64_t , BufferHandle handle) -> void { Destroy(handle); });
    _skinnedScratch.Clear();
}

void GeometryManager::ReleaseMeshBuffers() {
    _meshes.ForEach([this](AssetID, const Mesh& mesh) {
        const std::array buffers = {mesh.posBuffer,          mesh.attrBuffer,     mesh.skinBuffer,   mesh.indexBuffer,
                                    mesh.meshletBuffer, mesh.meshletVertexBuffer, mesh.meshletTriBuffer};
        for (const BufferHandle handle: buffers) {
            Destroy(handle);
        }
    });
    _meshes.Clear();
}

void GeometryManager::ReleaseParticleBuffers() {
    _particleBuffers.ForEach([this](uint64_t , const auto& tracked) -> void { Destroy(tracked.second); });
    _particleBuffers.Clear();
}

void GeometryManager::ReleaseLedgers() {
    for (auto* ledger: {&_emitters2D, &_emitters3D, &_entityBuffers}) {
        for (const auto& tracked: *ledger) {
            Destroy(tracked.second);
        }
        ledger->clear();
    }
}

template <typename DeadFn>
void GeometryManager::SweepLedgers(DeadFn&& isDead) {
    using namespace ZHLN::Ranges;

    auto sweep = [this, &isDead](auto& ledger) {
        ledger | EraseIf([this, &isDead](const auto& tracked) {
            if (isDead(tracked.first)) {
                Destroy(tracked.second);
                return true;
            }
            return false;
        });
    };
    sweep(_emitters2D);
    sweep(_emitters3D);
    sweep(_entityBuffers);

    ZHLN::Array<uint64_t> deadKeys;
    _particleBuffers.ForEach([&](uint64_t key, const auto& tracked) {
        if (isDead(tracked.first)) {
            Destroy(tracked.second);
            deadKeys.push_back(key);
        }
    });
    for (const uint64_t key: deadKeys) {
        _particleBuffers.Erase(key);
    }
}

void GeometryManager::ReleaseOwner(uint64_t packedOwner) {
    SweepLedgers([packedOwner](uint64_t owner) noexcept { return owner == packedOwner; });
}

void GeometryManager::Reconcile(EntityAliveQuery alive) {
    SweepLedgers([alive](uint64_t owner) { return !alive(Entity::Unpack(owner)); });
}

void GeometryManager::Destroy(BufferHandle handle) {
    if (handle != BufferHandle::Invalid) {
        Vk::ScopedDeletionQueue guard(_deletionQueue);
        _buffers.Destroy(handle);
    }
}

}
