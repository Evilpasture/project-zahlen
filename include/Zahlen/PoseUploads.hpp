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
#include <cstdint>
#include <span>
#include <vector>

namespace ZHLN {

struct PoseUpload {
    // First joint matrix this upload writes, in the renderer's skinning buffer.
    uint32_t                jointOffset = 0;
    std::vector<JPH::Mat44> matrices;
};

class PoseUploadQueue {
  public:
    // Copies: the producer's palette is usually frame-local storage that dies
    // before the renderer runs.
    void Push(uint32_t jointOffset, std::span<const JPH::Mat44> matrices) {
        PoseUpload upload {};
        upload.jointOffset = jointOffset;
        upload.matrices.assign(matrices.begin(), matrices.end());
        _uploads.push_back(std::move(upload));
    }

    [[nodiscard]] auto Size() const noexcept -> size_t {
        return _uploads.size();
    }

    // Renderer side. One thread: the simulation graph has finished before this
    // runs, so no lock is needed; the queue is empty again afterwards.
    [[nodiscard]] auto Take() noexcept -> std::vector<PoseUpload> {
        std::vector<PoseUpload> taken;
        taken.swap(_uploads);
        return taken;
    }

  private:
    std::vector<PoseUpload> _uploads;
};

} // namespace ZHLN
