// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Viewport.hpp
#pragma once
#include <cstdint>

namespace ZHLN {

/// How an extra Engine window is presented. The primary swapchain is always
/// the live scene graph; extras opt in.
///
/// This enum lives on its own rather than in Render.hpp so that a caller which
/// only needs to *name* a mode -- Engine::AddWindow -- does not drag in the
/// whole renderer surface to do it.
enum class ViewportMode : uint8_t {
    /// Clear/blit the live HDR frame and draw the current UI queue. UI editor
    /// Preview: document chrome, not a second 3D camera.
    UIOnly = 1,
    /// Mirror the primary window's resolved 3D output. No independent cull.
    BlitPrimary,
    /// Re-record the scene graph for this window's camera after the primary
    /// fence, reusing G-buffer/HDR targets (sequential, lowest VRAM).
    SceneCamera,
};

} // namespace ZHLN
