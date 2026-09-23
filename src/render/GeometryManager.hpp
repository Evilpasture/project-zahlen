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
//   * Usage flags come from the caller. Whether a buffer may feed an
//     acceleration structure depends on `Vk::RayTracingContext`, which the
//     render context owns and which is not constructed when this manager is;
//     adding that bit unconditionally would violate its VUID on hardware
//     without the feature. So the caller decides and this class obeys.
//   * Injection only: the device context, the allocator, the transfer staging
//     ring and command ring, and the deletion queue that defers retirement.
//     It never reaches back through RenderContext.
//
// Deliberately not here: the skinned-scratch buffers. Creating one writes the
// ray-tracing context's *address* into the NativeMesh so its BLAS is torn down
// with it, which is a dependency on the object rather than on a capability
// flag, and it is declared after this manager. Those stay with the render
// context alongside the asset caches and the entity buffer reconciliation.

#pragma once
#include "DrawCommands.hpp" // NativeMesh: what a BufferHandle resolves to
#include "GenerationalPool.hpp"
#include "Rendering.hpp"
#include <Zahlen/Render/Handles.hpp>
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

  private:
    Vk::Context&                                 _ctx;
    Vk::Allocator&                               _allocator;
    Vk::StagingRingBuffer&                       _transferRing;
    Vk::CommandRing<Vk::QueueType::Transfer, 8>& _transferCmdRing;
    Vk::DeletionQueue&                           _deletionQueue;

    // 8192 live buffers: vertex, index, storage, skinned scratch and particle
    // buffers all share this table, so the cap is a whole-scene budget rather
    // than a per-kind one.
    GenerationalPool<NativeMesh, 8192, BufferHandle> _buffers;
};

} // namespace ZHLN
