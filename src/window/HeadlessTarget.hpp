// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/window/HeadlessTarget.hpp
//
// A presentation target with no window system behind it: an offscreen extent
// the renderer draws into and nothing presents.
//
// This is the target a session that never opens a window should be handed.
// ZHLN::Window(headless = true) still carries its own headless branch, because
// the engine's window also owns the input receiver and the run-loop state that
// a bare target has no business holding; this class is the version that does
// not need GLFW at all, which is what a tool, a test or a dedicated offscreen
// render wants.

#pragma once

#include "PresentationTarget.hpp"
#include <cstdint>

namespace ZHLN {

class HeadlessPresentationTarget final: public IPresentationTarget {
  public:
    // The extent is the target's own and stays whatever it was set to: there is
    // no compositor to disagree with it.
    HeadlessPresentationTarget(uint32_t width, uint32_t height) noexcept;
    ~HeadlessPresentationTarget() override;

    HeadlessPresentationTarget(const HeadlessPresentationTarget&)                    = delete;
    auto operator=(const HeadlessPresentationTarget&) -> HeadlessPresentationTarget& = delete;

    [[nodiscard]] auto GetFramebufferExtent() const noexcept -> Extent2D override;
    void               SetFramebufferExtent(uint32_t width, uint32_t height) noexcept override;
    [[nodiscard]] auto GetNativeSurface() const noexcept -> const NativeSurfaceHandle& override;
    [[nodiscard]] auto IsHeadless() const noexcept -> bool override;
    [[nodiscard]] auto IsTTY() const noexcept -> bool override;
    void               Close() const noexcept override;

    // A headless session has nothing to close, so Close() only records that one
    // was asked for; a caller polling this gets the same answer a window's run
    // loop would.
    [[nodiscard]] auto WasClosed() const noexcept -> bool;

  private:
    // Built in the .cpp: the handle's body is this subsystem's private type.
    NativeSurfaceHandle _surface;
    Extent2D            _extent {.width = 0, .height = 0};
    // mutable: Close() is const (see IPresentationTarget), and this is exactly
    // the run state behind the target that it is allowed to touch.
    mutable bool _closed = false;
};

} // namespace ZHLN
