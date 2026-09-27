// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include "DrawCommands.hpp"
#include "GenerationalPool.hpp"
#include "Rendering.hpp"
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Error.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

namespace ZHLN {

class GeometryManager {
  public:
    GeometryManager(
        Vk::Context&                                ctx,
        Vk::Allocator&                              allocator,
        Vk::StagingRingBuffer&                      transferRingBuffer,
        Vk::CommandRing<Vk::QueueType::Transfer, 8>& transferCmdRing,
        Vk::DeletionQueue&                          deletionQueue
    ) noexcept
        : _ctx(ctx), _allocator(allocator), _transferRing(transferRingBuffer), _transferCmdRing(transferCmdRing), _deletionQueue(deletionQueue) {}
    ~GeometryManager() = default;

    GeometryManager(const GeometryManager&)                = delete;
    auto operator=(const GeometryManager&) -> GeometryManager& = delete;
    GeometryManager(GeometryManager&&) noexcept            = delete;
    auto operator=(GeometryManager&&) noexcept -> GeometryManager& = delete;


    [[nodiscard]] auto CreateBuffer(size_t size, const void* data, Vk::BufferUsage usage) const
        -> std::expected<std::pair<Vk::Buffer, VkDeviceAddress>, ErrorCode>;

    [[nodiscard]] auto CreateVertexBuffer(const void* data, size_t size, uint32_t stride, Vk::BufferUsage usage) -> BufferHandle;
    [[nodiscard]] auto CreateIndexBuffer(const void* data, size_t size, Vk::BufferUsage usage) -> BufferHandle;
    [[nodiscard]] auto CreateStorageBuffer(size_t size, Vk::BufferUsage usage) -> BufferHandle;
    [[nodiscard]] auto CreateStorageBuffer(const void* data, size_t size, uint32_t stride, Vk::BufferUsage usage) -> BufferHandle;

    [[nodiscard]] auto Adopt(Vk::Buffer&& buffer, uint32_t vertexCount, VkDeviceAddress address) -> BufferHandle;


    void Update(BufferHandle handle, const void* data, size_t size) noexcept;

    void Destroy(BufferHandle handle);

    using ResolveError = GenerationalPool<NativeMesh, 8192, BufferHandle>::Error;
    [[nodiscard]] auto Resolve(BufferHandle handle) const noexcept -> std::expected<NativeMesh*, ResolveError> { return _buffers.Resolve(handle); }


    void RegisterMesh(AssetID id, Mesh mesh) { _meshes.Insert(id, mesh); }
    void RegisterMaterial(MaterialID id, Material material) { _materials.Insert(id, material); }

    [[nodiscard]] auto FindMesh(AssetID id) const noexcept -> const Mesh* { return _meshes.Find(id); }
    [[nodiscard]] auto FindMaterial(MaterialID id) const noexcept -> const Material* { return _materials.Find(id); }

    void ReleaseMeshBuffers();
    void ClearMeshes() noexcept { _meshes.Clear(); }

    template <typename Fn>
    void ForEachMaterial(Fn&& fn) {
        _materials.ForEach(std::forward<Fn>(fn));
    }
    void ClearMaterials() noexcept { _materials.Clear(); }

    void ReleaseParticleBuffers();
    void ReleaseLedgers();


    void TrackEmitter2D(uint64_t packedOwner, BufferHandle buffer) { _emitters2D.push_back({packedOwner, buffer}); }
    void TrackEmitter3D(uint64_t packedOwner, BufferHandle buffer) { _emitters3D.push_back({packedOwner, buffer}); }
    void TrackEntityBuffer(uint64_t packedOwner, BufferHandle buffer) { _entityBuffers.push_back({packedOwner, buffer}); }

    [[nodiscard]] auto Emitters2D() noexcept -> ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>>& { return _emitters2D; }
    [[nodiscard]] auto Emitters3D() noexcept -> ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>>& { return _emitters3D; }
    [[nodiscard]] auto EntityBufferCount() const noexcept -> size_t { return _entityBuffers.size(); }

    void ReleaseOwner(uint64_t packedOwner);

    void Reconcile(EntityAliveQuery alive);


    [[nodiscard]] auto GetOrCreateParticleBuffer(uint64_t cacheKey, uint64_t packedOwner, size_t byteSize, Vk::BufferUsage usage) -> BufferHandle;
    void             ClearParticleBuffers();


    [[nodiscard]] auto CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle;
    [[nodiscard]] auto GetOrCreateSkinnedScratchBuffer(uint64_t entityKey, uint32_t vertexCount) -> BufferHandle;
    void             ReleaseSkinnedScratchBuffers();

  private:
    template <typename DeadFn>
    void SweepLedgers(DeadFn&& isDead);

    Vk::Context&                                 _ctx;
    Vk::Allocator&                               _allocator;
    Vk::StagingRingBuffer&                       _transferRing;
    Vk::CommandRing<Vk::QueueType::Transfer, 8>& _transferCmdRing;
    Vk::DeletionQueue&                           _deletionQueue;

    GenerationalPool<NativeMesh, 8192, BufferHandle> _buffers;

    ZHLN::HashMap<AssetID, Mesh>        _meshes;
    ZHLN::HashMap<MaterialID, Material> _materials;

    ZHLN::HashMap<uint64_t, ZHLN::Pair<uint64_t, BufferHandle>> _particleBuffers;

    ZHLN::HashMap<uint64_t, BufferHandle> _skinnedScratch;

    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>> _emitters2D;
    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>> _emitters3D;
    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>> _entityBuffers;
};

}
