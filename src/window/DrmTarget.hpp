// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/window/DrmTarget.hpp
//
// A presentation target that is a KMS/DRM connector with no window system in
// front of it -- the TTY session's half of the presentation seam.
//
// The descriptor it hands over is a DrmTarget: the card fd and the connector and
// CRTC the session owns. Vulkan does not build the surface from those; a
// direct-to-display surface comes from the physical device through VK_KHR_display
// (see Vk::CreateDisplaySurface), and the fd's job is to say which card the
// session is on so the RHI asks for the display extension and nothing else.
//
// Opening the card, taking the lease and mode-setting stay where they are
// (src/engine/tty); this class carries what the presentation side needs to know
// about the result, and no GLFW is involved in any of it.

#pragma once

#include <Zahlen/PresentationTarget.hpp>
#include <cstdint>

namespace ZHLN {

class DrmPresentationTarget final: public IPresentationTarget {
  public:
    // fd is the DRM card the session holds, or -1 for a target that only names
    // an extent; connectorId/crtcId are 0 when the session has not claimed one.
    DrmPresentationTarget(int fd, uint32_t connectorId, uint32_t crtcId, uint32_t width, uint32_t height) noexcept;
    ~DrmPresentationTarget() override;

    DrmPresentationTarget(const DrmPresentationTarget&)                    = delete;
    auto operator=(const DrmPresentationTarget&) -> DrmPresentationTarget& = delete;

    [[nodiscard]] auto GetFramebufferExtent() const noexcept -> Extent2D override;
    // The connector owns the mode, so the extent is only updated when the
    // session mode-sets again; a caller asking for a resize gets the size it
    // asked for back and the real one on the next mode-set.
    void               SetFramebufferExtent(uint32_t width, uint32_t height) noexcept override;
    [[nodiscard]] auto GetNativeSurface() const noexcept -> const NativeSurfaceHandle& override;
    [[nodiscard]] auto IsHeadless() const noexcept -> bool override;
    [[nodiscard]] auto IsTTY() const noexcept -> bool override;
    void               Close() noexcept override;

    [[nodiscard]] auto WasClosed() const noexcept -> bool;
    [[nodiscard]] auto Fd() const noexcept -> int;

  private:
    NativeSurfaceHandle _surface;
    Extent2D            _extent {.width = 0, .height = 0};
    int                 _fd     = -1;
    bool                _closed = false;
};

} // namespace ZHLN
