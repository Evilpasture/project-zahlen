// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/GeometryManager.cpp

#include "GeometryManager.hpp"

#include <Zahlen/Core/Ranges.hpp>
#include <Zahlen/Vertex.hpp> // VertexPosition, VertexAttributes: what a skinned scratch buffer holds
#include <array>
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

    // The ray-tracing build-input bit is the manager's one usage-flag decision:
    // it depends only on whether the device enabled the RT extensions, which the
    // injected context already answers. Adding it unconditionally would violate
    // its VUID on hardware without the feature, so it rides on the predicate.
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
    const BufferHandle handle = _buffers.Create(std::move(buffer), vertexCount, address);
    // Every mesh in this table lives on this manager's device; the stamp is
    // what lets NativeMesh's destructor retire a BLAS with no other reference.
    if (auto* mesh = _buffers.Resolve(handle).value_or(nullptr)) {
        mesh->device = _ctx.Device();
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

    // The skinning dispatch writes it and the RT passes read it as BLAS input;
    // nothing stages initial contents into it, so it bypasses CreateBuffer's
    // transfer path. The build-input bit rides on the same predicate as there.
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
    _skinnedScratch.ForEach([this](uint64_t /*key*/, BufferHandle handle) -> void { Destroy(handle); });
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
    _particleBuffers.ForEach([this](uint64_t /*key*/, const auto& tracked) -> void { Destroy(tracked.second); });
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

    // The particle cache is keyed by subresource rather than by owner, so the
    // owner is carried in the value and the dead keys have to be collected
    // before erasing -- the map cannot be mutated inside its own ForEach.
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
        // Defer destruction for 2 frames so the GPU finishes reading from the
        // buffer. The pool releases the Vk::Buffer; the scoped guard routes it
        // into the deletion queue instead of destroying it inline.
        Vk::ScopedDeletionQueue guard(_deletionQueue);
        _buffers.Destroy(handle);
    }
}

} // namespace ZHLN
