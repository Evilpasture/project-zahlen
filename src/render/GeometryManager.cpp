// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GeometryManager.hpp"
#include <Zahlen/Vertex.hpp>
#include <cstring>
#include <format>
#include <optional>

namespace ZHLN {

namespace {

// The ways a BufferSource can contradict itself, all rejected before anything
// reaches Vulkan. Kept next to its only caller for the same reason
// BufferCreationError and BufferMapError are: nothing outside this TU names it.
enum class BufferSourceError : uint8_t {
    EmptySource     ZHLN_ANNOTATION(ZHLN::Description<"Buffer source has neither bytes nor an allocation size"> {}) = 1,
    ZeroStride      ZHLN_ANNOTATION(ZHLN::Description<"Buffer source declares a zero-sized element"> {}),
    ConflictingSize ZHLN_ANNOTATION(ZHLN::Description<"Buffer source sets bytes and an allocation size; the bytes already decide the size"> {}),
    RaggedElements  ZHLN_ANNOTATION(ZHLN::Description<"Buffer source byte count is not a whole number of its elements"> {}),
};

// The order is deliberate: a source carrying no payload at all is reported as such
// before its stride is blamed, and a size stated twice is reported before its
// divisibility is judged.
[[nodiscard]] auto SourceError(const GeometryManager::BufferSource& source) noexcept -> std::optional<BufferSourceError> {
    if (source.bytes.empty() && source.allocationSize == 0) {
        return BufferSourceError::EmptySource;
    }
    if (source.stride == 0) {
        return BufferSourceError::ZeroStride;
    }
    if (!source.bytes.empty() && source.allocationSize != 0) {
        return BufferSourceError::ConflictingSize;
    }
    if (!source.bytes.empty() && (source.bytes.size() % source.stride) != 0) {
        return BufferSourceError::RaggedElements;
    }
    return std::nullopt;
}

// The ring reports "no room for this upload" by handing back an allocation whose
// mapped pointer is null, which every caller then has to re-test. Fold that into the
// error channel once. It lives in this TU rather than in StagingRingBuffer because
// the ring's other consumers (RenderResources.cpp, TextureUploader.hpp) still read
// Allocation::mappedData; the follow-up step moves it to the source and deletes this
// helper. See todo/TODO.md.
[[nodiscard]] auto AllocateStaging(Vk::StagingRingBuffer& ring, size_t size) noexcept
    -> std::expected<Vk::StagingRingBuffer::Allocation, ErrorCode> {
    auto allocation = ring.Allocate(size);
    if (allocation.mappedData == nullptr) {
        return std::unexpected(Vk::StagingError::StagingSpaceExhausted);
    }
    return allocation;
}

} // namespace

auto GeometryManager::CreateBuffer(const BufferSource& source, Vk::BufferUsage usage) -> std::expected<BufferHandle, ErrorCode> {
    if (const auto contradiction = SourceError(source)) {
        return std::unexpected(*contradiction);
    }

    const size_t            size       = source.TotalSize();
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
        .and_then(
            [this, size, bytes = source.bytes, elementCount = source.ElementCount()](Vk::Buffer gpu_buf) -> std::expected<BufferHandle, ErrorCode> {
                defer _([&] { _allocator.DestroyBuffer(gpu_buf); });
                auto staging = AllocateStaging(_transferRing, size);
                if (!staging) {
                    return std::unexpected(staging.error());
                }

                if (bytes.empty()) {
                    std::memset(staging->mappedData, 0, size);
                } else {
                    std::memcpy(staging->mappedData, bytes.data(), size);
                }

                Vk::ExecuteImmediate<Vk::QueueType::Transfer>(_ctx, _transferCmdRing, _transferRing, [&](VkCommandBuffer cmd) -> void {
                    Vk::CopyRingBuffer(cmd, *staging, gpu_buf);
                });

                const VkDeviceAddress address = _ctx.BufferAddress(gpu_buf.Handle());
                return Adopt(std::move(gpu_buf), elementCount, address);
            }
        );
}

auto GeometryManager::Adopt(Vk::Buffer buffer, uint32_t vertexCount, VkDeviceAddress address) -> BufferHandle {
    const BufferHandle handle = _buffers.Create(std::move(buffer), vertexCount, address);
    if (handle == BufferHandle::Invalid) {
        _allocator.DestroyBuffer(buffer); // Pool full: Create did not take the buffer, so this frame's copy is all there is. The caller's
                                           // buffer was moved-from on the way in (Buffer is move-only), so its own DestroyBuffer is a no-op.
    } else if (NativeMesh* mesh = _buffers.Resolve(handle)) {
        Vk::Debug::SetBufferName(_ctx, mesh->buffer.Handle(), std::format("GeometryBuffer#{}", static_cast<uint64_t>(handle)));
    }
    return handle;
}

void GeometryManager::Update(BufferHandle handle, const void* data, size_t size) noexcept {
    if (handle == BufferHandle::Invalid || data == nullptr || size == 0) {
        return;
    }
    auto* nativeMesh = _buffers.Resolve(handle);
    if (nativeMesh == nullptr) {
        return;
    }

    auto staging = AllocateStaging(_transferRing, size);
    if (!staging) {
        return;
    }
    std::memcpy(staging->mappedData, data, size);

    Vk::ExecuteImmediate<Vk::QueueType::Transfer>(_ctx, _transferCmdRing, _transferRing, [&](VkCommandBuffer cmd) -> void {
        Vk::CopyRingBuffer(cmd, *staging, nativeMesh->buffer);
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
            const VkDeviceAddress address = _ctx.BufferAddress(gpu_buf.Handle());
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
