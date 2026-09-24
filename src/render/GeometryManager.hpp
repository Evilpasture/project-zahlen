// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/GeometryManager.hpp
//
// The lifetime of every GPU buffer the renderer holds: allocate one, stage its
// initial contents, hand back a generational handle, upload through it later,
// and retire it without pulling it out from under a frame still reading it.
//
//   * The handle table is the point. A `BufferHandle` packs a generation and a
//     slot index, so a handle outliving its buffer fails the generation check
//     and resolves to nothing rather than onto the slot's new occupant -- the
//     same stale-handle discipline DestinationRegistry applies to windows.
//   * Usage flags come from the caller, except the ray-tracing one: whether
//     a buffer may feed an acceleration structure build depends only on the
//     device's enabled extensions, which the injected context answers
//     (`RayTracingSupported()`), so CreateBuffer adds the bit itself and no
//     caller spells it.
//   * Injection only: the device context, the allocator, the transfer staging
//     ring and command ring, and the deletion queue that defers retirement.
//     It never reaches back through RenderContext.
//
// The skinned-scratch buffers live here with the other buffer caches. Their
// BLAS teardown used to need a ray-tracing context pointer inside the
// NativeMesh; now the destructor needs only the device the mesh already
// carries, and adoption stamps it, so nothing in this cache reaches outside
// the manager.

#pragma once
#include "DrawCommands.hpp" // NativeMesh: what a BufferHandle resolves to
#include "GenerationalPool.hpp"
#include "Rendering.hpp"
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Entity.hpp> // EntityAliveQuery: how an owner is found to be gone
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/Render/Types.hpp> // Mesh, Material: what the asset caches hold
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

    // --- Allocation --------------------------------------------------------

    // Creates a GPU-only buffer carrying `usage` plus the bits every buffer
    // here needs (transfer destination, shader device address), and stages
    // `data` into it -- or zeroes it when `data` is null, because an
    // uninitialised buffer is read by the temporal passes on its first frame.
    //
    // Returns the buffer and its device address without registering either:
    // the caller decides what the buffer is and therefore how many vertices it
    // represents, then adopts it.
    [[nodiscard]] auto CreateBuffer(size_t size, const void* data, Vk::BufferUsage usage) const
        -> std::expected<std::pair<Vk::Buffer, VkDeviceAddress>, ErrorCode>;

    [[nodiscard]] auto CreateVertexBuffer(const void* data, size_t size, uint32_t stride, Vk::BufferUsage usage) -> BufferHandle;
    [[nodiscard]] auto CreateIndexBuffer(const void* data, size_t size, Vk::BufferUsage usage) -> BufferHandle;
    [[nodiscard]] auto CreateStorageBuffer(size_t size, Vk::BufferUsage usage) -> BufferHandle;
    [[nodiscard]] auto CreateStorageBuffer(const void* data, size_t size, uint32_t stride, Vk::BufferUsage usage) -> BufferHandle;

    // Registers an already-created buffer in the table. `vertexCount` is what
    // the skinned and BLAS paths read back off the handle, so it is the
    // caller's to state rather than something to derive from a byte count.
    // Stamps the mesh's device with this manager's: NativeMesh's destructor
    // retires a BLAS off that stamp alone, with no reach-back to any context.
    [[nodiscard]] auto Adopt(Vk::Buffer&& buffer, uint32_t vertexCount, VkDeviceAddress address) -> BufferHandle;

    // --- Use and retirement ------------------------------------------------

    // Re-uploads `size` bytes over the start of a live buffer through the
    // transfer ring. A stale or invalid handle is ignored rather than an error:
    // callers are ECS systems that may legitimately outlive the buffer they
    // were handed.
    void Update(BufferHandle handle, const void* data, size_t size) noexcept;

    // Releases the handle. The buffer itself is retired through the deletion
    // queue, not destroyed here, so a frame still reading it finishes first.
    void Destroy(BufferHandle handle);

    // Same signature the pool has always had, so the 25 call sites keep
    // spelling `.value_or(nullptr)` and the three that inspect *why* a handle
    // failed keep getting a reason. Const method, mutable mesh: the passes
    // resolve a handle off a const context reference and then write through the
    // mesh, which is the pool's existing contract rather than a new one.
    using ResolveError = GenerationalPool<NativeMesh, 8192, BufferHandle>::Error;
    [[nodiscard]] auto Resolve(BufferHandle handle) const noexcept -> std::expected<NativeMesh*, ResolveError> { return _buffers.Resolve(handle); }

    // --- Asset caches ------------------------------------------------------
    // What the engine has already uploaded, so a second load of the same asset
    // resolves instead of re-uploading. Keyed by the engine's asset identity;
    // this class never sees a path or a file.

    void RegisterMesh(AssetID id, Mesh mesh) { _meshes.Insert(id, mesh); }
    void RegisterMaterial(MaterialID id, Material material) { _materials.Insert(id, material); }

    [[nodiscard]] auto FindMesh(AssetID id) const noexcept -> const Mesh* { return _meshes.Find(id); }
    [[nodiscard]] auto FindMaterial(MaterialID id) const noexcept -> const Material* { return _materials.Find(id); }

    // Retires every buffer the cached meshes hold, then drops the cache. This
    // is the buffer half of a cache clear; the caller must already have made
    // the device idle, because the meshes are about to stop existing.
    void ReleaseMeshBuffers();
    void ClearMeshes() noexcept { _meshes.Clear(); }

    // A cached material owns pipelines rather than buffers, so pipeline
    // retirement is not this class's to do. The render context walks the cache
    // itself and then clears it -- the iteration is exposed, the retirement is
    // not, and that line is where the pipeline registry will pick this up.
    template <typename Fn>
    void ForEachMaterial(Fn&& fn) {
        _materials.ForEach(std::forward<Fn>(fn));
    }
    void ClearMaterials() noexcept { _materials.Clear(); }

    // Retires the particle cache and the three entity ledgers, buffers and all.
    void ReleaseParticleBuffers();
    void ReleaseLedgers();

    // --- Per-entity buffer ledgers -----------------------------------------
    // Buffers whose lifetime is an entity's, not a frame's: particle emitters
    // and per-entity storage. The renderer keeps three ledgers because the
    // particle systems reconcile the first two on their own schedule while the
    // third is swept wholesale, so they cannot be merged into one list without
    // changing who owns the sweep.
    //
    // Owners arrive already packed. Unpacking an id back into an Entity is the
    // caller's business; a geometry manager has no reason to know the shape of
    // an entity beyond the 64 bits it is keyed by.

    void TrackEmitter2D(uint64_t packedOwner, BufferHandle buffer) { _emitters2D.push_back({packedOwner, buffer}); }
    void TrackEmitter3D(uint64_t packedOwner, BufferHandle buffer) { _emitters3D.push_back({packedOwner, buffer}); }
    void TrackEntityBuffer(uint64_t packedOwner, BufferHandle buffer) { _entityBuffers.push_back({packedOwner, buffer}); }

    [[nodiscard]] auto Emitters2D() noexcept -> ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>>& { return _emitters2D; }
    [[nodiscard]] auto Emitters3D() noexcept -> ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>>& { return _emitters3D; }
    [[nodiscard]] auto EntityBufferCount() const noexcept -> size_t { return _entityBuffers.size(); }

    // Destroys every buffer the ledgers attribute to one owner. Despawn wants
    // this immediately rather than at the next reconcile, because the entity is
    // already gone and its components with it.
    void ReleaseOwner(uint64_t packedOwner);

    // Sweeps all four ledgers, destroying the buffers of owners the query says
    // are dead. This is the only place a buffer owned by a dead entity is
    // reclaimed without an explicit despawn.
    void Reconcile(EntityAliveQuery alive);

    // --- Particle buffers --------------------------------------------------
    // Cache-keyed by the caller: the key folds the owner and the subresource,
    // and the byte size is computed from the particle struct, which is a shader
    // ABI type this class has no business knowing.

    [[nodiscard]] auto GetOrCreateParticleBuffer(uint64_t cacheKey, uint64_t packedOwner, size_t byteSize, Vk::BufferUsage usage) -> BufferHandle;
    void             ClearParticleBuffers();

    // --- Skinned scratch ----------------------------------------------------
    // The pos+attr buffer the skinning dispatch writes and the RT passes read
    // as BLAS input. Created without staging -- nothing is uploaded into it,
    // the compute pass writes it on its first use -- and keyed per entity so a
    // re-skin reuses last frame's buffer.

    [[nodiscard]] auto CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle;
    [[nodiscard]] auto GetOrCreateSkinnedScratchBuffer(uint64_t entityKey, uint32_t vertexCount) -> BufferHandle;
    void             ReleaseSkinnedScratchBuffers();

  private:
    // The two sweeps above differ only in which owners count as dead.
    template <typename DeadFn>
    void SweepLedgers(DeadFn&& isDead);

    Vk::Context&                                 _ctx;
    Vk::Allocator&                               _allocator;
    Vk::StagingRingBuffer&                       _transferRing;
    Vk::CommandRing<Vk::QueueType::Transfer, 8>& _transferCmdRing;
    Vk::DeletionQueue&                           _deletionQueue;

    // 8192 live buffers: vertex, index, storage, skinned scratch and particle
    // buffers all share this table, so the cap is a whole-scene budget rather
    // than a per-kind one.
    GenerationalPool<NativeMesh, 8192, BufferHandle> _buffers;

    ZHLN::HashMap<AssetID, Mesh>        _meshes;
    ZHLN::HashMap<MaterialID, Material> _materials;

    // Cache-key -> {packed owner, buffer}. The owner is kept so a despawn can
    // find its particle buffers, which are keyed by subresource rather than by
    // owner and so cannot be looked up from the entity alone.
    ZHLN::HashMap<uint64_t, ZHLN::Pair<uint64_t, BufferHandle>> _particleBuffers;

    // Entity key -> scratch buffer (see CreateSkinnedScratchBuffer).
    ZHLN::HashMap<uint64_t, BufferHandle> _skinnedScratch;

    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>> _emitters2D;
    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>> _emitters3D;
    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>> _entityBuffers;
};

} // namespace ZHLN
