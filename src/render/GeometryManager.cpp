// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GeometryManager.hpp"
#include <Zahlen/Vertex.hpp>
#include <algorithm>
#include <cstring>
#include <format>
#include <optional>

namespace ZHLN {

namespace {

enum class BufferSourceError : uint8_t {
    EmptySource     ZHLN_ANNOTATION(ZHLN::Description<"Buffer source has neither bytes nor an allocation size"> {}) = 1,
    ZeroStride      ZHLN_ANNOTATION(ZHLN::Description<"Buffer source declares a zero-sized element"> {}),
    ConflictingSize ZHLN_ANNOTATION(ZHLN::Description<"Buffer source sets bytes and an allocation size; the bytes already decide the size"> {}),
    RaggedElements  ZHLN_ANNOTATION(ZHLN::Description<"Buffer source byte count is not a whole number of its elements"> {}),
};

enum class BufferUpdateError : uint8_t {
    InvalidHandle   ZHLN_ANNOTATION(ZHLN::Description<"Buffer update received an invalid or unknown buffer handle"> {}) = 1,
    NullData        ZHLN_ANNOTATION(ZHLN::Description<"Buffer update received a null data pointer"> {}),
    EmptyData       ZHLN_ANNOTATION(ZHLN::Description<"Buffer update received zero bytes"> {}),
    ExceedsCapacity ZHLN_ANNOTATION(ZHLN::Description<"Buffer update byte count exceeds destination buffer capacity"> {}),
};

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

// TODO(Evilpasture): The ring reports "no room for this upload"
// by handing back an allocation whose mapped pointer is null, which every caller
// then has to re-test. Fold that into the error channel once. It lives in this
// TU rather than in StagingRingBuffer because the ring's other consumers
// (RenderResources.cpp, TextureUploader.hpp) still read Allocation::mappedData;
// the follow-up step moves it to the source and deletes this helper.
[[nodiscard]] auto AllocateStaging(Vk::StagingRingBuffer& ring, size_t size) noexcept -> std::expected<Vk::StagingRingBuffer::Allocation, ErrorCode> {
    auto allocation = ring.Allocate(size);
    if (allocation.mappedData == nullptr) {
        return std::unexpected(Vk::StagingError::StagingSpaceExhausted);
    }
    return allocation;
}

} // namespace

auto GeometryManager::AllocateGpuBuffer(size_t size, Vk::BufferUsage usage) -> std::expected<Vk::Buffer, ErrorCode> {
    const auto&             familyInfo = _ctx.PhysicalInfo();
    const std::array        candidates = {familyInfo.graphicsFamily, familyInfo.transferFamily, familyInfo.computeFamily};
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
    const auto rtBit       = _ctx.RayTracingSupported() ? Vk::BufferUsage::AccelerationStructureBuildInput : Vk::BufferUsage::None;

    return Vk::Buffer::Create(
        _allocator, size, usage | rtBit | Vk::BufferUsage::TransferDst | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly, 0, sharingMode,
        {families.data(), familyCount}
    );
}

auto GeometryManager::CreateBuffer(const BufferSource& source, Vk::BufferUsage usage) -> std::expected<BufferHandle, ErrorCode> {
    if (const auto contradiction = SourceError(source)) {
        return std::unexpected(*contradiction);
    }

    const size_t size = source.TotalSize();
    return AllocateGpuBuffer(size, usage)
        .and_then([this, size, bytes = source.bytes, elementCount = source.ElementCount()](Vk::Buffer gpu_buf) -> std::expected<BufferHandle, ErrorCode> {
            defer _([&] { _allocator.DestroyBuffer(gpu_buf); });
            auto  staging = AllocateStaging(_transferRing, size);
            if (!staging) {
                return std::unexpected(staging.error());
            }

            if (bytes.empty()) {
                std::memset(staging->mappedData, 0, size);
            } else {
                std::memcpy(staging->mappedData, bytes.data(), size);
            }

            if (auto immediate = Vk::ExecuteImmediate<Vk::QueueType::Transfer>(_ctx, _transferCmdRing, _transferRing, [&](VkCommandBuffer cmd) -> void {
                Vk::CopyRingBuffer(cmd, *staging, gpu_buf);
            }); !immediate) [[unlikely]] {
                return std::unexpected(immediate.error());
            }

            const Vk::DeviceAddress<> address {_ctx.BufferAddress(gpu_buf.Handle())};
            return Adopt(std::move(gpu_buf), elementCount, address);
        });
}

auto GeometryManager::CreateUninitializedBuffer(size_t allocationSize, Vk::BufferUsage usage, uint32_t elementCount)
    -> std::expected<BufferHandle, ErrorCode> {
    if (allocationSize == 0) {
        return std::unexpected(BufferSourceError::EmptySource);
    }

    return AllocateGpuBuffer(allocationSize, usage).transform([this, elementCount](Vk::Buffer gpu_buf) -> BufferHandle {
        const Vk::DeviceAddress<> address {_ctx.BufferAddress(gpu_buf.Handle())};
        return Adopt(std::move(gpu_buf), elementCount, address);
    });
}

auto GeometryManager::Adopt(Vk::Buffer buffer, uint32_t vertexCount, Vk::DeviceAddress<> address) -> BufferHandle {
    const BufferHandle handle = _buffers.Create(std::move(buffer), vertexCount, address);
    if (handle == BufferHandle::Invalid) {
        _allocator.DestroyBuffer(buffer);

    } else if (NativeMesh* mesh = _buffers.Resolve(handle)) {
        Vk::Debug::SetBufferName(_ctx, mesh->buffer.Handle(), std::format("GeometryBuffer#{}", static_cast<uint64_t>(handle)));
    }
    return handle;
}

auto GeometryManager::Update(BufferHandle handle, const void* data, size_t size) noexcept -> std::expected<void, ErrorCode> {
    if (handle == BufferHandle::Invalid) {
        return std::unexpected(BufferUpdateError::InvalidHandle);
    }
    if (data == nullptr) {
        return std::unexpected(BufferUpdateError::NullData);
    }
    if (size == 0) {
        return std::unexpected(BufferUpdateError::EmptyData);
    }
    auto* nativeMesh = _buffers.Resolve(handle);
    if (nativeMesh == nullptr) {
        return std::unexpected(BufferUpdateError::InvalidHandle);
    }
    if (size > nativeMesh->buffer.Size()) {
        return std::unexpected(BufferUpdateError::ExceedsCapacity);
    }

    constexpr size_t kUploadAlignment   = 16u;
    constexpr size_t kMinUploadCapacity = 256u * 1024u;

    UploadSlot& slot          = _uploadSlots[_currentUploadSlot];
    size_t      alignedOffset = (slot.offset + (kUploadAlignment - 1u)) & ~(kUploadAlignment - 1u);

    if (!slot.buffer.Valid() || alignedOffset + size > slot.buffer.Size()) {
        const size_t newCapacity = std::max({size, slot.buffer.Size() * 2u, kMinUploadCapacity});
        auto         newStaging  = Vk::Buffer::Create(_allocator, newCapacity, Vk::BufferUsage::TransferSrc, Vk::MemoryUsage::CPUOnly);
        if (!newStaging) {
            return std::unexpected(newStaging.error());
        }
        Vk::Debug::SetBufferName(_ctx, newStaging->Handle(), std::format("BufferUploadRing.Slot{}", _currentUploadSlot));
        if (slot.buffer.Valid()) {
            _deletionQueue.Enqueue(std::move(slot.buffer));
        }
        slot.buffer   = std::move(*newStaging);
        slot.offset   = 0;
        alignedOffset = 0;
    }

    {
        auto mapped = slot.buffer.Map(_allocator);
        if (!mapped) {
            return std::unexpected(mapped.error());
        }
        std::memcpy(mapped->As<std::byte>() + alignedOffset, data, size);
    }
    slot.offset = alignedOffset + size;

    _pendingCopies.push_back(PendingBufferCopy {
        .srcBuffer = slot.buffer.Handle(),
        .dstHandle = handle,
        .srcOffset = static_cast<VkDeviceSize>(alignedOffset),
        .size      = static_cast<VkDeviceSize>(size),
    });
    return {};
}

void GeometryManager::RecordPendingUpdates(VkCommandBuffer cmd) noexcept {
    if (_pendingCopies.empty() || cmd == VK_NULL_HANDLE) {
        return;
    }

    bool                  recordedAny = false;
    std::vector<VkBuffer> writtenBuffers;
    writtenBuffers.reserve(_pendingCopies.size());

    for (const PendingBufferCopy& copy: _pendingCopies) {
        NativeMesh* dstMesh = _buffers.Resolve(copy.dstHandle);
        if (dstMesh == nullptr || !dstMesh->buffer.Valid() || copy.size > dstMesh->buffer.Size()) {
            continue;
        }
        const VkBuffer dstBuffer = dstMesh->buffer.Handle();
        if (std::ranges::find(writtenBuffers, dstBuffer) != writtenBuffers.end()) {
            Vk::MemoryBarrier::CopyWriteToWrite().Record(cmd);
        } else {
            writtenBuffers.push_back(dstBuffer);
        }

        Vk::CopyBuffer(cmd, copy.srcBuffer, dstBuffer, copy.size, copy.srcOffset, 0);
        recordedAny = true;
    }

    _pendingCopies.clear();

    if (!recordedAny) {
        return;
    }

    VkPipelineStageFlags2 dstStage =
        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_2_COPY_BIT;
    VkAccessFlags2 dstAccess =
        VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT |
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
        VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (_ctx.RayTracingSupported()) {
        dstStage |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        dstAccess |= VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    }

    Vk::MemoryBarrier {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, dstStage, dstAccess}.Record(cmd);
}

void GeometryManager::AdvanceUploadFrame() noexcept {
    if (!_pendingCopies.empty()) {
        return;
    }
    _currentUploadSlot                     = (_currentUploadSlot + 1u) % kUploadRingSize;
    _uploadSlots[_currentUploadSlot].offset = 0;
}

void GeometryManager::DiscardPendingUpdates() noexcept {
    _pendingCopies.clear();
    for (auto& slot: _uploadSlots) {
        slot.offset = 0;
    }
}

auto GeometryManager::CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle {
    const size_t size = (static_cast<size_t>(vertexCount) * sizeof(VertexPosition)) + (static_cast<size_t>(vertexCount) * sizeof(VertexTangentFrame));

    Vk::BufferUsage usage = Vk::BufferUsage::Vertex | Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress;
    if (_ctx.RayTracingSupported()) {
        usage |= Vk::BufferUsage::AccelerationStructureBuildInput;
    }

    return Vk::Buffer::Create(_allocator, size, usage, Vk::MemoryUsage::GPUOnly)
        .transform([this, vertexCount](auto&& gpu_buf) -> BufferHandle {
            const Vk::DeviceAddress<> address {_ctx.BufferAddress(gpu_buf.Handle())};
            return Adopt(std::forward<decltype(gpu_buf)>(gpu_buf), vertexCount, address);
        })
        .value_or(BufferHandle::Invalid);
}

void GeometryManager::Retire(NativeMesh& mesh) noexcept {
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
    DiscardPendingUpdates();
    for (auto& slot: _uploadSlots) {
        if (slot.buffer.Valid()) {
            _deletionQueue.Enqueue(std::move(slot.buffer));
        }
        slot.offset = 0;
    }
    _buffers.ForEachLive([this](NativeMesh& mesh) { Retire(mesh); });
}

} // namespace ZHLN
