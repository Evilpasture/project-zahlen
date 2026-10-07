// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Skinning palettes: the simulation produces them, the renderer uploads them.
//
// A palette is not a description the renderer can derive -- it is the pose this
// frame's solvers produced -- but the *upload* is the renderer's job. So the
// simulation pushes palettes here and the renderer drains them, instead of a
// simulation system holding a renderer reference to call UpdateJointMatrices.
//
// Order is meaningful: producers run in graph order and the renderer applies the
// uploads in the order they arrived, so a later pose (articulation over
// procedural) wins over an earlier one for the same region.

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Zahlen/Core/Arena.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace ZHLN {

struct PoseUpload {
    // First joint matrix this upload writes, in the renderer's skinning buffer.
    uint32_t jointOffset = 0;
    // Queue-owned storage, valid through the Drain() callbacks and invalid afterwards.
    std::span<const JPH::Mat44> matrices;
};

class PoseUploadQueue {
  public:
    PoseUploadQueue() {
        _uploads.reserve(kInitialUploadCount);
    }

    ~PoseUploadQueue() noexcept {
        DestroyQueuedMatrices();
    }

    PoseUploadQueue(const PoseUploadQueue&)                    = delete;
    auto operator=(const PoseUploadQueue&) -> PoseUploadQueue& = delete;
    PoseUploadQueue(PoseUploadQueue&&)                         = delete;
    auto operator=(PoseUploadQueue&&) -> PoseUploadQueue&      = delete;

    // Copies into queue-owned arenas. The producer's matrices may be frame or
    // task scratch and need not remain alive through Present.
    void Push(uint32_t jointOffset, std::span<const JPH::Mat44> matrices) {
        PoseUpload upload {};
        upload.jointOffset = jointOffset;
        if (!matrices.empty()) {
            auto* destination = static_cast<JPH::Mat44*>(Allocate(matrices.size_bytes(), alignof(JPH::Mat44)));
            std::uninitialized_copy(matrices.begin(), matrices.end(), destination);
            upload.matrices = {destination, matrices.size()};
        }
        _uploads.push_back(upload);
    }

    [[nodiscard]] auto Size() const noexcept -> size_t {
        return _uploads.size();
    }

    // Renderer side. Producers have finished before this is called, so no lock
    // is needed. Every matrix remains alive for the complete consumer call; all
    // queued matrix objects are destroyed and the arenas reset afterwards.
    template <typename Consumer>
    void Drain(Consumer&& consume) {
        for (const PoseUpload& upload: _uploads) {
            std::invoke(consume, upload);
        }
        DestroyQueuedMatrices();
        _uploads.clear();
        for (LinearArena& arena: _arenas) {
            arena.Reset();
        }
        _currentArena = 0;
    }

  private:
    static constexpr size_t kInitialArenaBytes = 64 * 1024;
    static constexpr size_t kInitialUploadCount = 16;

    [[nodiscard]] static auto CanAllocate(const LinearArena& arena, size_t bytes, size_t alignment) noexcept -> bool {
        const size_t offset = Math::AlignUp(arena.AllocatedBytes(), alignment);
        return offset <= arena.Capacity() && bytes <= arena.Capacity() - offset;
    }

    [[nodiscard]] static auto NewArenaCapacity(size_t bytes, size_t alignment) -> size_t {
        const size_t max = std::numeric_limits<size_t>::max();
        if (bytes > max - (alignment - 1)) {
            Panic("Pose upload allocation overflows size_t: {} bytes", bytes);
        }
        return std::max(kInitialArenaBytes, bytes + alignment - 1);
    }

    [[nodiscard]] auto Allocate(size_t bytes, size_t alignment) -> void* {
        if (alignment > GetPageSize()) {
            Panic("PoseUploadQueue cannot satisfy matrix alignment {} (page size {})", alignment, GetPageSize());
        }
        if (_arenas.empty()) {
            // Delay page commitment until there is an upload, and size the first
            // block for it so a large first palette doesn't strand a small one.
            _arenas.emplace_back(NewArenaCapacity(bytes, alignment));
        }

        while (_currentArena < _arenas.size() && !CanAllocate(_arenas[_currentArena], bytes, alignment)) {
            ++_currentArena;
        }
        if (_currentArena == _arenas.size()) {
            // LinearArena is movable and its backing pages keep their addresses,
            // so growing this metadata vector cannot invalidate earlier uploads.
            _arenas.emplace_back(NewArenaCapacity(bytes, alignment));
        }
        return _arenas[_currentArena].Allocate(bytes, alignment);
    }

    void DestroyQueuedMatrices() noexcept {
        for (const PoseUpload& upload: _uploads) {
            if (!upload.matrices.empty()) {
                std::destroy_n(const_cast<JPH::Mat44*>(upload.matrices.data()), upload.matrices.size());
            }
        }
    }

    // The queue owns the matrix objects until Drain; upload spans never point
    // into worker scratch. Keep the arena list and upload descriptors separate:
    // arena segments can grow without moving any already-published matrix data.
    std::vector<LinearArena> _arenas;
    std::vector<PoseUpload>  _uploads;
    size_t                   _currentArena = 0;
};

} // namespace ZHLN
