// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/PipelineRegistry.hpp"

#include <Zahlen/Log.hpp>
#include <utility>

namespace ZHLN {

void PipelineRegistry::Destroy(PipelineID handle) {
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
    kLog.Warn("Mesh pipeline creation failed ({}); this material keeps the vertex pipeline.", error);
}

}
