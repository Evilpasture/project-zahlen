// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "DrawCommands.hpp"
#include "GenerationalPool.hpp"
#include "Rendering.hpp"
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/Render/Types.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

namespace ZHLN {

class GeometryManager {
  public:
    GeometryManager(
        Vk::Context&                                 ctx,
        Vk::Allocator&                               allocator,
        Vk::StagingRingBuffer&                       transferRingBuffer,
        Vk::CommandRing<Vk::QueueType::Transfer, 8>& transferCmdRing,
        Vk::DeletionQueue&                           deletionQueue
    ) noexcept: _ctx(ctx), _allocator(allocator), _transferRing(transferRingBuffer), _transferCmdRing(transferCmdRing), _deletionQueue(deletionQueue) {
    }
    ~GeometryManager() {
        RetireAll();
    }

    GeometryManager(const GeometryManager&)                        = delete;
    auto operator=(const GeometryManager&) -> GeometryManager&     = delete;
    GeometryManager(GeometryManager&&) noexcept                    = delete;
    auto operator=(GeometryManager&&) noexcept -> GeometryManager& = delete;

    // A buffer's payload, named instead of spread over positional arguments.
    // Describes either the bytes to upload or an allocation to zero-fill, and binds
    // the element size to the byte count so the two cannot disagree: a source whose
    // bytes are not a whole number of its elements is rejected rather than counted
    // wrong.
    struct BufferSource {
        std::span<const std::byte> bytes          = {};
        size_t                     allocationSize = 0; // read when `bytes` is empty
        uint32_t                   stride         = 1; // bytes per element

        [[nodiscard]] auto TotalSize() const noexcept -> size_t {
            return bytes.empty() ? allocationSize : bytes.size();
        }
        // Zero for an allocation-only source: nothing has been uploaded, so there are
        // no elements to count yet. That is what this pool recorded for its
        // allocation-only storage buffers before (Adopt(..., 0, ...)), unchanged.
        [[nodiscard]] auto ElementCount() const noexcept -> uint32_t {
            return (bytes.empty() || stride == 0) ? 0U : static_cast<uint32_t>(bytes.size() / stride);
        }
    };

    // Create, upload and adopt in one step. The named creators this replaces differed
    // only in the usage bits and in what they divided the byte count by, and both now
    // ride on the arguments: the usage as a flag, the element size as the source's
    // stride. A source that contradicts itself is reported as an ErrorCode (see
    // BufferSourceError in GeometryManager.cpp) instead of reaching Vulkan.
    [[nodiscard]] auto CreateBuffer(const BufferSource& source, Vk::BufferUsage usage) -> std::expected<BufferHandle, ErrorCode>;

    [[nodiscard]] auto Adopt(Vk::Buffer buffer, uint32_t vertexCount, VkDeviceAddress address) -> BufferHandle;

    void Update(BufferHandle handle, const void* data, size_t size) noexcept;

    void Destroy(BufferHandle handle);

    [[nodiscard]] auto Resolve(BufferHandle handle) const noexcept -> NativeMesh* {
        return _buffers.Resolve(handle);
    }

    // AssetID lookup only: the creator of the buffers owns their lifetime.
    void RegisterMesh(AssetID id, Mesh mesh) {
        _meshes.Insert(id, mesh);
    }
    void UnregisterMesh(AssetID id) {
        _meshes.Erase(id);
    }
    void RegisterMaterial(MaterialID id, Material material) {
        _materials.Insert(id, material);
    }
    void UnregisterMaterial(MaterialID id) {
        _materials.Erase(id);
    }

    [[nodiscard]] auto FindMesh(AssetID id) const noexcept -> ZHLN::Optional<const Mesh&> {
        return _meshes.Find(id);
    }
    [[nodiscard]] auto FindMaterial(MaterialID id) const noexcept -> ZHLN::Optional<const Material&> {
        return _materials.Find(id);
    }

    void ClearMeshes() noexcept {
        _meshes.Clear();
    }

    void ClearMaterials() noexcept {
        _materials.Clear();
    }

    [[nodiscard]] auto CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle;
    // After GPU idle the renderer drains the queue before releasing the allocator.
    void RetireAll() noexcept;

  private:
    void Retire(NativeMesh& mesh) noexcept;

    Vk::Context&                                 _ctx;
    Vk::Allocator&                               _allocator;
    Vk::StagingRingBuffer&                       _transferRing;
    Vk::CommandRing<Vk::QueueType::Transfer, 8>& _transferCmdRing;
    Vk::DeletionQueue&                           _deletionQueue;

    // The geometry throughput scene holds 1,600 distinct boxes, each with
    // position, tangent-frame, surface, and three meshlet buffers: 9,600 live
    // handles before other scene resources. Keep headroom for other streams.
    GenerationalPool<NativeMesh, 16384, BufferHandle> _buffers;

    ZHLN::HashMap<AssetID, Mesh>        _meshes;
    ZHLN::HashMap<MaterialID, Material> _materials;
};

} // namespace ZHLN
