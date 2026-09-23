// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/window/NativeSurfaceInternal.hpp
//
// The private half of the presentation bridge.
//
// This is the one header two subsystems share: src/window/ fills a
// NativeSurfaceHandle in, src/vulkan/ reads one out. Everything OS-specific the
// engine knows about a window lives in the variant below and nowhere else --
// not in a public header, not in the renderer, and not in the RHI's public
// headers.
//
// The two sides stay decoupled because each only ever touches the alternatives
// it understands: the window side constructs one alternative per platform, and
// the RHI visits them with its own Vk*SurfaceCreateInfo builders. Adding a
// platform is one struct here, one constructor branch in Window.cpp and one
// visitor arm in src/vulkan/presentation/Surface.cpp; no other file changes.
//
// It is included by path spelling ("NativeSurfaceInternal.hpp", resolved
// through the src/window include directory) rather than "window/..." so that
// the cross-subsystem include boundary stays exactly this one file.
#pragma once

#include "PresentationTarget.hpp"
#include <cstdint>
#include <type_traits>
#include <utility>
#include <variant>

namespace ZHLN {

// --- The platform descriptors. Opaque pointers on purpose: the window side
// never casts these, and the RHI casts each one exactly once, into the create
// info that consumes it. ---

struct Win32Target {
    void* hwnd      = nullptr;
    void* hinstance = nullptr;
};

struct WaylandTarget {
    void* display = nullptr;
    void* surface = nullptr;
};

struct X11Target {
    void* display = nullptr;
    // An X11 window is an XID, which is `unsigned long` in Xlib's ABI on every
    // platform that has one. Kept as the raw width rather than as a Window
    // typedef so this header needs no Xlib include.
    unsigned long window = 0;
};

struct CocoaTarget {
    // macOS has no native Vulkan WSI: sessions there present through the
    // host-blit plugin's own OpenGL window, so this is filled in only by a
    // caller that supplied a CAMetalLayer and is left null by the GLFW path.
    void* caMetalLayer = nullptr;
};

// Direct-to-display (KMS/DRM), the TTY session's descriptor. Vulkan builds this
// surface from the physical device through VK_KHR_display rather than from the
// fd, so the fields identify the connector the session is on rather than feed a
// create info.
struct DrmTarget {
    int      fd          = -1;
    uint32_t connectorId = 0;
    uint32_t crtcId      = 0;
};

// No window system at all: nothing to present to, so the RHI creates no
// VkSurfaceKHR and requests no WSI extension.
struct HeadlessTarget {};

using NativeSurfaceVariant = std::variant<HeadlessTarget, Win32Target, WaylandTarget, X11Target, CocoaTarget, DrmTarget>;

struct NativeSurfaceHandle::Impl {
    NativeSurfaceVariant target;

    // One converting constructor for every alternative. Constrained so it
    // cannot also match Impl itself: an unconstrained `Impl(T&&)` is a better
    // match for `Impl&` than the implicitly-declared copy constructor is, which
    // would silently turn a copy into a re-wrap and leave the original empty.
    template <typename T>
        requires(!std::is_same_v<std::remove_cvref_t<T>, Impl>)
    explicit Impl(T&& t): target(std::forward<T>(t)) {
    }
};

// std::visit's missing helper: one overload set out of a pack of lambdas, so a
// visitor can name only the alternatives it handles and take the rest through a
// `[](const auto&)` arm.
template <class... Ts>
struct Overloaded: Ts... {
    using Ts::operator()...;
};

// The read side of the bridge: dispatch on what the window side stored. Visit()
// is a friend of NativeSurfaceHandle (see the grant in PresentationTarget.hpp),
// which is how it reaches the handle's private PIMPL: a descriptor is read by
// this one visitor, and the class has no accessor that would let anyone else do
// the same.
template <typename Visitor>
decltype(auto) Visit(const NativeSurfaceHandle& handle, Visitor&& visitor) {
    // The pointee is cast back to const on purpose: a handle read through a
    // const reference must not hand a visitor a mutable descriptor, which is
    // what unique_ptr::operator-> on a const handle would do by itself.
    const NativeSurfaceHandle::Impl& impl = *handle._impl;
    return std::visit(std::forward<Visitor>(visitor), impl.target);
}

} // namespace ZHLN
