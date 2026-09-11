// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Types.hpp>
#include <cstdint>

namespace ZHLN {

/// Geometry sink for Clay's EndFrameAndRender. UIRenderer implements this;
/// RenderContext forwards after waiting extra viewports.
class IUISubmitter {
  public:
    virtual ~IUISubmitter() = default;

    virtual void SubmitUI(
        const UIBatch*          batches,
        uint32_t                batchCount,
        const VertexPosition*   positions,
        const VertexAttributes* attributes,
        uint32_t                vertexCount
    ) noexcept = 0;
};

} // namespace ZHLN
