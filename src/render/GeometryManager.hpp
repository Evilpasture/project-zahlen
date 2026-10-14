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

    struct BufferSource {
        std::span<const std::byte> bytes          = {};
        size_t                     allocationSize = 0;
        uint32_t                   stride         = 1;

        [[nodiscard]] auto TotalSize() const noexcept -> size_t {
            return bytes.empty() ? allocationSize : bytes.size();
        }

        [[nodiscard]] auto ElementCount() const noexcept -> uint32_t {
            return (bytes.empty() || stride == 0) ? 0U : static_cast<uint32_t>(bytes.size() / stride);
        }
    };

    [[nodiscard]] auto CreateBuffer(const BufferSource& source, Vk::BufferUsage usage) -> std::expected<BufferHandle, ErrorCode>;

    [[nodiscard]] auto Adopt(Vk::Buffer buffer, uint32_t vertexCount, Vk::DeviceAddress<> address) -> BufferHandle;

    [[nodiscard]] auto Update(BufferHandle handle, const void* data, size_t size) noexcept -> std::expected<void, ErrorCode>;

    void Destroy(BufferHandle handle);

    [[nodiscard]] auto Resolve(BufferHandle handle) const noexcept -> NativeMesh* {
        return _buffers.Resolve(handle);
    }

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

    void RetireAll() noexcept;

  private:
    void Retire(NativeMesh& mesh) noexcept;

    Vk::Context&                                 _ctx;
    Vk::Allocator&                               _allocator;
    Vk::StagingRingBuffer&                       _transferRing;
    Vk::CommandRing<Vk::QueueType::Transfer, 8>& _transferCmdRing;
    Vk::DeletionQueue&                           _deletionQueue;

    GenerationalPool<NativeMesh, 16384, BufferHandle> _buffers;

    ZHLN::HashMap<AssetID, Mesh>        _meshes;
    ZHLN::HashMap<MaterialID, Material> _materials;
};

} // namespace ZHLN
