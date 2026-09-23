// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Error.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/gui/UIData.hpp>
#include <cstdint>
#include <expected>
#include <memory>

namespace ZHLN::Vk {
class CommandEncoder;
}

namespace ZHLN {

// Self-contained 2D UI pass: own pipeline, own VBOs, own heap mappings
// (sampler + texture array). Does not bind GlobalSceneRegistry.
//
// Renderer-private by design: the public UI surface is
// `RenderContext::RenderUI(UIView, UIDrawData)`, which the facade forwards to
// the UI pipeline. Being private is what lets Init/Record take
// renderer-internal types (RenderContext::Impl, Vk::CommandEncoder) directly
// instead of going through an access shim.
//
// The renderer does not queue geometry: `Record` consumes a `UIDrawData`
// payload (spans the GUI context owns for the frame) straight into the
// double-buffered staging VBO and draws it, so there is no CPU-side copy that
// outlives its producer.
class UIRenderer {
  public:
    UIRenderer();
    ~UIRenderer();

    UIRenderer(UIRenderer&&) noexcept;
    auto operator=(UIRenderer&&) noexcept -> UIRenderer&;
    UIRenderer(const UIRenderer&)                    = delete;
    auto operator=(const UIRenderer&) -> UIRenderer& = delete;

    // Builds pipeline/buffers/heap mappings against a live render context.
    auto Init(RenderContext::Impl& ctx) -> std::expected<void, ErrorCode>;

    // Marks the start of a frame. The slot a `Record` addresses is
    // double-buffered, but a frame is not: one frame calls `RenderUI` once per
    // window it draws UI into, and every one of those calls appends after the
    // last instead of overwriting it. Rewinding the slot on its first `Record`
    // of a frame is what makes that work; without it the editor's chrome and a
    // preview window's chrome are the same vertices.
    void BeginFrame() noexcept;

    // Uploads `uiData` into this frame's VBO slot and draws it. A no-op while
    // empty, uninitialized, or degenerate in size.
    void Record(Vk::CommandEncoder& encoder, uint32_t width, uint32_t height, uint32_t frameIndex, const UIDrawData& uiData) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
