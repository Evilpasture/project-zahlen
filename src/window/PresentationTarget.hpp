// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/window/PresentationTarget.hpp
//
// The presentation seam between "something can show pixels" and "something can
// draw them".
//
// Two types live here and nothing else does:
//
//   * PresentationTarget -- a concrete, non-virtual value type. It carries the
//     four things a renderer asks of whatever it is drawing into (how big am I,
//     what kind of target am I, what is my native presentation descriptor, end
//     this session) and the two function pointers that let a window answer
//     dynamically without the target knowing what a window is.
//   * NativeSurfaceHandle -- an opaque PIMPL token for that descriptor. What it
//     actually holds is an OS-specific struct (HWND, wl_surface*, XID, a KMS
//     fd, ...); every one of them is sealed inside the windowing subsystem's
//     private NativeSurfaceInternal.hpp, and the variant that holds them is
//     visited there and by src/render's surface creation, never here.
//
// There is no interface here and no vtable. The set of presentation targets is
// closed -- desktop window, KMS/DRM console, offscreen -- so the kind is a
// value in the object and the per-kind behaviour is a function pointer the
// producer installed, which is the same shape GPUDiagnostics takes over its
// trackers and NativeSurfaceHandle takes over the platform descriptors. Nothing
// outside src/window/ needs to name a concrete producer, and nothing here is
// inherited from, so the -fno-rtti / -Wweak-vtables constraints that shaped the
// interface this replaced are simply not in play any more.
//
// This is an internal service type, not public API. It lives in src/window/
// because the engine's two audiences need opposite things from it: a game client
// wants a Window and never hears the phrase "presentation target", while
// zahlen_render and the swapchain want a decoupled seam with no GLFW in it. Both
// are served by keeping this out of include/Zahlen/: the renderer's low-level
// verbs take one (RenderContext.hpp forward-declares the type for exactly that),
// and the kernel -- which owns the session and every window in it -- asks the
// host for the target privately, then hands its caller an attachment. Nothing
// above RenderContext names the type, and nothing hands one out.
//
// The header is still deliberately sterile. It names no OS type, no GLFW type
// and no Vulkan type, and it pulls in no engine header beyond the two that
// define ZHLN_API and Extent2D. Keeping Zahlen/Error.hpp, Zahlen/Types.hpp and
// Zahlen/Config.hpp out of it is what makes that true in practice rather than
// by inspection: through them this file would drag in the reflection macros,
// <format> and the Jolt math headers, and every consumer of the presentation
// seam would pay the engine's whole header cost for one forward-declared
// struct.
//
// One invariant a producer must keep: a PresentationTarget is the identity key
// the render layer's destination registry hashes on (DestinationRegistry::Find
// compares entry.target == &target). Once one has been handed to a renderer its
// address has to stay put for the lifetime of that renderer -- which is why the
// producers all hold theirs by member and never move it after installation.
#pragma once

#include <Zahlen/Common.h>       // ZHLN_API
#include <Zahlen/Geometry2D.hpp> // Extent2D
#include <cstdint>
#include <memory>
#include <utility>

namespace ZHLN {

class NativeSurfaceHandle;

// The read half of the seam, and the only way into a handle. Declared here so
// the friend grant inside NativeSurfaceHandle names a template that already
// exists -- a friend declaration cannot introduce one -- and defined in
// src/window/NativeSurfaceInternal.hpp, where the descriptor variant behind a
// handle is complete.
//
// It exists because the alternative, an accessor returning the PIMPL, is the
// hole this header no longer has. A consumer that wants the descriptor hands a
// visitor to Visit(); nothing else reaches it, because nothing else is a friend
// of the class.
template <typename Visitor>
decltype(auto) Visit(const NativeSurfaceHandle& handle, Visitor&& visitor);

// Opaque handle to a native presentation descriptor.
//
// The producer (src/window/) fills it in from whatever the OS gave it; the
// reader (src/render/PresentationSurface.cpp) visits it and turns it into a
// VkSurfaceKHR. Neither side sees the other's headers, and neither is included
// here: Impl is declared and never defined in this translation unit, so this
// header compiles identically on every platform.
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

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _impl != nullptr;
    }

  private:
    // This class has one reader, and it is the visitor declared above: it is the
    // friend grant that lets src/render's surface creation reach the descriptor
    // behind a handle, and the reason no accessor has to exist. Everything else
    // that wants a handle's contents -- including the windowing side that built
    // it -- goes through the public half of this class or the target that owns
    // it.
    template <typename Visitor>
    friend decltype(auto) Visit(const NativeSurfaceHandle& handle, Visitor&& visitor);

    std::unique_ptr<Impl> _impl;
};

// Which of the three things this target is. The set is closed, so a value is
// enough -- there is nothing for a caller to override and no fourth case for a
// consumer to have forgotten.
enum class TargetKind : uint8_t {
    Window,   // a desktop window behind a window system
    TTY,      // a Linux console presenting straight to a KMS/DRM connector
    Headless, // offscreen: an extent the renderer draws into and nothing presents
};

// What a renderer is given to draw into.
//
// ZHLN::Window composes one and hands it out; the console and offscreen
// sessions build one directly. The renderer holds a reference to it and never
// names the producer, which is what keeps the window system out of it.
//
// Movable so a producer can install one by factory, never copyable because it
// owns the descriptor it names.
class ZHLN_API PresentationTarget {
  public:
    // The dynamic halves. A window's framebuffer extent belongs to the
    // compositor, so it has to be asked rather than stored; ending the session
    // has to reach back to whoever can actually end it. Both take the opaque
    // userdata the producer passed in, and both are plain function pointers
    // rather than members of an interface -- the target does not know what a
    // GLFWwindow is and never will.
    using ExtentFn = Extent2D (*)(void* userdata) noexcept;
    using CloseFn  = void (*)(void* userdata) noexcept;

    // The empty target. Headless with no extent and no descriptor, which is
    // what a producer holds before it has something real to publish.
    PresentationTarget() noexcept = default;
    ~PresentationTarget()         = default;

    PresentationTarget(PresentationTarget&& other) noexcept;
    auto operator=(PresentationTarget&& other) noexcept -> PresentationTarget&;

    // Owns its descriptor and one producer's state, so a copy would be two
    // owners of both.
    PresentationTarget(const PresentationTarget&)                    = delete;
    auto operator=(const PresentationTarget&) -> PresentationTarget& = delete;

    // A desktop window. extentFn asks the compositor for the real framebuffer
    // size on every call; surface is usually empty at construction and filled
    // by SetNativeSurface once the window exists.
    [[nodiscard]] static auto ForWindow(void* userdata, ExtentFn extentFn, CloseFn closeFn, NativeSurfaceHandle surface = {}) noexcept -> PresentationTarget {
        return PresentationTarget(TargetKind::Window, userdata, extentFn, closeFn, std::move(surface), {});
    }

    // Offscreen. No producer behind it, so the extent is its own and stays
    // whatever SetFramebufferExtent last made it, and there is no descriptor to
    // hand over -- GetNativeSurface() is the empty handle and Valid() says so.
    [[nodiscard]] static auto ForHeadless(Extent2D extent, NativeSurfaceHandle surface = {}) noexcept -> PresentationTarget {
        return PresentationTarget(TargetKind::Headless, nullptr, nullptr, nullptr, std::move(surface), extent);
    }

    // A console presenting straight to a KMS/DRM connector. The connector owns
    // the mode, so the extent is stored rather than asked for, and the
    // descriptor is the card fd the session holds.
    [[nodiscard]] static auto ForTTY(void* userdata, CloseFn closeFn, Extent2D extent, NativeSurfaceHandle surface) noexcept -> PresentationTarget {
        return PresentationTarget(TargetKind::TTY, userdata, nullptr, closeFn, std::move(surface), extent);
    }

    // Physical framebuffer extent in pixels -- points on a Retina display, not
    // the window's logical size. Zero in either axis means "no drawable area
    // right now" (minimised), which is a state a frame skips, not an error.
    [[nodiscard]] auto GetFramebufferExtent() const noexcept -> Extent2D {
        return _extentFn != nullptr ? _extentFn(_userdata) : _staticExtent;
    }

    // Advisory resize of the target's own framebuffer. Only a target that owns
    // its size acts on it: a windowed target's size belongs to the compositor,
    // so _extentFn shadows whatever is written here and callers guard this with
    // IsHeadless(). That shadowing is the same behaviour the windowed producer
    // always had, where the stored size was written and never read back.
    void SetFramebufferExtent(uint32_t width, uint32_t height) noexcept {
        _staticExtent = {.width = width, .height = height};
    }

    // The opaque token the RHI turns into a VkSurfaceKHR. Visited once at
    // instance/surface creation and again on a rebuild, never per frame: the
    // frame path holds the swapchain handles it built from it.
    [[nodiscard]] auto GetNativeSurface() const noexcept -> const NativeSurfaceHandle& {
        return _surface;
    }

    // Producer-side only: swaps the descriptor in place, on the address this
    // target already has, so the identity a renderer is keyed on survives a
    // reconnect or a device-lost rebuild. The render layer never calls this;
    // it only ever reads GetNativeSurface().
    void SetNativeSurface(NativeSurfaceHandle surface) noexcept {
        _surface = std::move(surface);
    }

    [[nodiscard]] auto Kind() const noexcept -> TargetKind {
        return _kind;
    }

    // True when there is no window system in this session at all: nothing to
    // present to, so no surface is created and no WSI extension is requested.
    [[nodiscard]] auto IsHeadless() const noexcept -> bool {
        return _kind == TargetKind::Headless;
    }

    // True when the session presents straight to a KMS/DRM connector with no
    // window system between them. The surface is built from the physical device
    // rather than from a native window handle.
    [[nodiscard]] auto IsTTY() const noexcept -> bool {
        return _kind == TargetKind::TTY;
    }

    // Asks the target to end its session: the owner's run loop reads this back
    // as "not running" on its next poll. Used by a presenter that owns its own
    // on-screen window (the macOS host-blit path) and has to end the session
    // when that window is closed.
    //
    // const because a caller that holds the target only by const reference -- the
    // destination registry, which does not own what it keys on -- still has to be
    // able to end the session. What changes is run state behind the producer's
    // own back, not anything a reader observes as the target's shape, so this is
    // the logical kind of constness.
    void Close() const noexcept {
        if (_closeFn != nullptr) {
            _closeFn(_userdata);
        }
        _closed = true;
    }

    // Whether Close() has been called. Offscreen has nothing to shut down and a
    // console's text mode is restored by its owner, so for both of them this is
    // the only record that an end was asked for, and it is what their run loops
    // poll.
    [[nodiscard]] auto WasClosed() const noexcept -> bool {
        return _closed;
    }

  private:
    PresentationTarget(TargetKind kind, void* userdata, ExtentFn extentFn, CloseFn closeFn, NativeSurfaceHandle surface, Extent2D extent) noexcept:
        _surface(std::move(surface)), _staticExtent(extent), _userdata(userdata), _extentFn(extentFn), _closeFn(closeFn), _kind(kind) {
    }

    // Move is member-wise, including the move-only handle. Not defaulted in the
    // header because it has to move a member whose definition this header only
    // declares -- see PresentationTarget.cpp.
    NativeSurfaceHandle _surface;
    Extent2D            _staticExtent {.width = 0, .height = 0};
    void*               _userdata = nullptr;
    ExtentFn            _extentFn = nullptr;
    CloseFn             _closeFn  = nullptr;
    TargetKind          _kind     = TargetKind::Headless;
    // mutable: Close() is const, and this is exactly the run state behind the
    // target that it is allowed to touch.
    mutable bool _closed = false;
};

} // namespace ZHLN
