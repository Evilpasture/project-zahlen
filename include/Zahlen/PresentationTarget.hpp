// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/PresentationTarget.hpp
//
// The presentation seam between "something can show pixels" and "something can
// draw them".
//
// Two types live here and nothing else does:
//
//   * IPresentationTarget -- the abstract surface a renderer is handed. It
//     answers four questions (how big am I, am I headless, am I a TTY, what is
//     my native presentation descriptor) and nothing more.
//   * NativeSurfaceHandle -- an opaque PIMPL token for that descriptor. What it
//     actually holds is an OS-specific struct (HWND, wl_surface*, XID, a KMS
//     fd, ...); every one of them is sealed inside the windowing subsystem's
//     private NativeSurfaceInternal.hpp, and the variant that holds them is
//     visited there and in the RHI, never here.
//
// This header is deliberately sterile. It names no OS type, no GLFW type and no
// Vulkan type, and it pulls in no engine header beyond the two that define
// ZHLN_API and Extent2D. Keeping Zahlen/Error.hpp, Zahlen/Types.hpp and
// Zahlen/Config.hpp out of it is what makes that true in practice rather than
// by inspection: through them this file would drag in the reflection macros,
// <format> and the Jolt math headers, and every consumer of the presentation
// seam would pay the engine's whole header cost for one forward-declared
// struct.
#pragma once

#include <Zahlen/Common.h>       // ZHLN_API
#include <Zahlen/Geometry2D.hpp> // Extent2D
#include <cstdint>
#include <memory>

namespace ZHLN {

// Opaque handle to a native presentation descriptor.
//
// The producer (src/window/) fills it in from whatever the OS gave it; the
// consumer (src/vulkan/) reads it back and turns it into a VkSurfaceKHR.
// Neither side sees the other's headers, and neither is included here: Impl is
// declared and never defined in this translation unit, so this header compiles
// identically on every platform.
//
// Movable, never copyable -- it owns the descriptor it names.
class ZHLN_API NativeSurfaceHandle {
  public:
    struct Impl;

    // The empty handle. Valid() is false, and a consumer that gets one answers
    // "unsupported" rather than dereferencing it.
    NativeSurfaceHandle() noexcept;
    explicit NativeSurfaceHandle(std::unique_ptr<Impl> impl) noexcept;
    ~NativeSurfaceHandle();

    NativeSurfaceHandle(NativeSurfaceHandle&& other) noexcept;
    auto operator=(NativeSurfaceHandle&& other) noexcept -> NativeSurfaceHandle&;

    NativeSurfaceHandle(const NativeSurfaceHandle&)                    = delete;
    auto operator=(const NativeSurfaceHandle&) -> NativeSurfaceHandle& = delete;

    // The PIMPL body. Defined out of line in src/window/NativeSurfaceHandle.cpp,
    // where Impl is complete; the visitors that read it include
    // src/window/NativeSurfaceInternal.hpp for that definition.
    [[nodiscard]] auto GetImpl() const noexcept -> const Impl&;

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _impl != nullptr;
    }

  private:
    std::unique_ptr<Impl> _impl;
};

// What a renderer is given to draw into.
//
// ZHLN::Window is the desktop implementation; HeadlessPresentationTarget and
// DrmPresentationTarget are the two that need no window system at all. The
// renderer holds a reference to this and never names the concrete type, which
// is what keeps the window system out of it.
class ZHLN_API IPresentationTarget {
  public:
    // Out of line on purpose: it is the class's key function, so the vtable is
    // emitted once, in src/window/PresentationTarget.cpp, instead of weakly in
    // every translation unit that includes this header (-Wweak-vtables).
    virtual ~IPresentationTarget();

    IPresentationTarget() noexcept                                     = default;
    IPresentationTarget(const IPresentationTarget&)                    = delete;
    auto operator=(const IPresentationTarget&) -> IPresentationTarget& = delete;

    // Physical framebuffer extent in pixels -- points on a Retina display, not
    // the window's logical size. Zero in either axis means "no drawable area
    // right now" (minimised), which is a state a frame skips, not an error.
    [[nodiscard]] virtual auto GetFramebufferExtent() const noexcept -> Extent2D = 0;

    // Advisory resize of the target's own framebuffer. Only a target that owns
    // its size acts on it: a windowed target's size belongs to the compositor,
    // so callers guard this with IsHeadless().
    virtual void SetFramebufferExtent(uint32_t width, uint32_t height) noexcept = 0;

    // The opaque token the RHI turns into a VkSurfaceKHR. Visited once at
    // instance/surface creation and again on a rebuild, never per frame: the
    // frame path holds the swapchain handles it built from it.
    [[nodiscard]] virtual auto GetNativeSurface() const noexcept -> const NativeSurfaceHandle& = 0;

    // True when there is no window system in this session at all: nothing to
    // present to, so no surface is created and no WSI extension is requested.
    [[nodiscard]] virtual auto IsHeadless() const noexcept -> bool = 0;

    // True when the session presents straight to a KMS/DRM connector with no
    // window system between them. The surface is built from the physical device
    // rather than from a native window handle.
    [[nodiscard]] virtual auto IsTTY() const noexcept -> bool = 0;

    // Asks the target to end its session: the owner's run loop reads this back
    // as "not running" on its next poll. Used by a presenter that owns its own
    // on-screen window (the macOS host-blit path) and has to end the session
    // when that window is closed.
    virtual void Close() noexcept = 0;
};

} // namespace ZHLN
