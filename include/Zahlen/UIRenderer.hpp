// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/UISubmitter.hpp>
#include <cstdint>
#include <memory>

namespace ZHLN {

struct UIRendererAccess;

/// Self-contained 2D UI pass: own pipeline, own VBOs, own heap mappings
/// (sampler + texture array). Does not bind GlobalSceneRegistry.
class UIRenderer final : public IUISubmitter {
  public:
    UIRenderer();
    ~UIRenderer() override;

    UIRenderer(UIRenderer&&) noexcept;
    auto operator=(UIRenderer&&) noexcept -> UIRenderer&;
    UIRenderer(const UIRenderer&)                    = delete;
    auto operator=(const UIRenderer&) -> UIRenderer& = delete;

    void SubmitUI(
        const UIBatch*          batches,
        uint32_t                batchCount,
        const VertexPosition*   positions,
        const VertexAttributes* attributes,
        uint32_t                vertexCount
    ) noexcept override;

    void Clear() noexcept;

    [[nodiscard]] auto Empty() const noexcept -> bool;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

    friend struct UIRendererAccess;
};

} // namespace ZHLN
