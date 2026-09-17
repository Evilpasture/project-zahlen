// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Error.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/UISubmitter.hpp>
#include <cstdint>
#include <expected>
#include <memory>

namespace ZHLN::Vk {
class CommandEncoder;
}

namespace ZHLN {

/// Self-contained 2D UI pass: own pipeline, own VBOs, own heap mappings
/// (sampler + texture array). Does not bind GlobalSceneRegistry.
///
/// Renderer-private by design: the public UI surface is IUISubmitter, which
/// RenderContext implements and forwards here. Being private is what lets
/// Init/Record take renderer-internal types (RenderContext::Impl,
/// Vk::CommandEncoder) directly instead of going through an access shim.
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

    /// Builds pipeline/buffers/heap mappings against a live render context.
    auto Init(RenderContext::Impl& ctx) -> std::expected<void, ErrorCode>;

    /// Draws the queued batches; a no-op while empty or uninitialized.
    void Record(Vk::CommandEncoder& encoder, uint32_t width, uint32_t height, uint32_t frameIndex) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
