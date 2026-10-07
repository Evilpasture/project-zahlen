// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PipelineRegistry.hpp"

#include <Zahlen/Log.hpp>
#include <utility>

namespace ZHLN {

void PipelineRegistry::Destroy(PipelineHandle handle) {
    if (_materials.Resolve(handle) != nullptr) {
        _materials.Destroy(handle);
    }
}

void PipelineRegistry::RetireAll() noexcept {
    _materials.Clear();
    for (Vk::Pipeline& pipeline: _sharedPipelines) {
        if (pipeline.Valid()) {
            _deletionQueue.EnqueuePipeline(_ctx.Device(), pipeline.Release());
        }
    }
    _meshVariantAttempted.fill(false);
}

void PipelineRegistry::LogMeshPipelineFallback(ErrorCode error) const noexcept {
    ZHLN::Log("[PipelineRegistry] Mesh pipeline creation failed ({}); this material keeps the vertex pipeline.", error);
}

}
