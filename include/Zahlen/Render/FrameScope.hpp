// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Render/FrameResult.hpp>
#include <Zahlen/Render/Info.hpp>
#include <expected>

namespace ZHLN {

class RenderContext;
struct SceneData;
struct SceneRenderPass;
struct SceneView;
struct UIView;
struct UIDrawData;
struct GraphicsSettings;

// Capability token for one GPU frame. BeginFrame returns it; End() presents.
// Destructor aborts (does not present) if End() was not called.
class ZHLN_API FrameScope {
  public:
    FrameScope() noexcept = default;
    FrameScope(FrameScope&& other) noexcept;
    auto operator=(FrameScope&& other) noexcept -> FrameScope&;
    ~FrameScope();

    FrameScope(const FrameScope&)                    = delete;
    auto operator=(const FrameScope&) -> FrameScope& = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return _rc != nullptr;
    }

    // Emitters go to the compute queue. Lines/billboards/debug tris wait for RenderScene.
    [[nodiscard]] auto DispatchSimulations(const SceneData& scene, float dt) noexcept -> RenderResult;

    [[nodiscard]] auto RenderScene(const SceneRenderPass& pass, const SceneData& scene) noexcept -> FrameOutcome<FrameSkipped>;

    [[nodiscard]] auto RenderUI(const UIView& view, const UIDrawData& uiData) noexcept -> FrameOutcome<FrameSkipped>;

    [[nodiscard]] auto End() && noexcept -> FrameOutcome<PresentSuboptimal>;

    // Drop the frame without presenting (errors, early return).
    void Cancel() noexcept;

  private:
    friend class RenderContext;
    explicit FrameScope(RenderContext& rc) noexcept;

    RenderContext* _rc    = nullptr;
    bool           _ended = false;
};

} // namespace ZHLN
