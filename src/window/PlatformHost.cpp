// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/window/PlatformHost.cpp

#include "NativeSurfaceInternal.hpp" // DrmTarget, NativeSurfaceHandle::Impl, Overloaded
#include "PresentationTarget.hpp"
#include "tty/TTYBackend.hpp"
#include <GLFW/glfw3.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/Window.hpp>
#include <memory>
#include <string>
#include <utility>
#include <variant>

namespace ZHLN {

// The three session shapes, as plain state rather than classes. Each one holds
// only what it actually has -- a window, a TTY lease, or an extent -- and
// PlatformHost's members dispatch across them with std::visit. Nothing here is
// polymorphic, which is the point: no vtable, no key function to keep the vtable
// out of every translation unit, and no virtual standing in for a dynamic_cast
// that -fno-rtti will not let anyone write.

namespace {

// Offscreen. No display, no event queue, no window system -- and no Window
// object anywhere in it, which is the whole point of the shape.
struct HeadlessBackend {
    PresentationTarget target;
    std::string        clipboard;
    bool               running = true;

    HeadlessBackend(uint32_t width, uint32_t height) noexcept: target(PresentationTarget::ForHeadless({.width = width, .height = height})) {
    }

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return true;
    }
    [[nodiscard]] auto IsRunning() const noexcept -> bool {
        return running && !target.WasClosed();
    }
    void PollEvents() noexcept {
        // No event source. Deliberately not a call into GLFW: a headless session
        // never initialised it.
    }
    void Close() const noexcept {
        target.Close();
    }
};

// Direct to display on a Linux console. Takes the TTY over through TTYBackend
// (libseat + libevdev) and presents through VK_KHR_display. GLFW is never
// initialised for one of these, and no Window is ever constructed.
struct TTYBackend {
    PresentationTarget  target;
    WindowInputReceiver receiver;
    void*               ttyContext = nullptr;
    std::string         clipboard;
    bool                closed = false;

    TTYBackend(uint32_t width, uint32_t height, const WindowInputReceiver& rx) noexcept: receiver(rx) {
        ttyContext = TTYBackend::Init(width, height);

        // The descriptor says which card the session is on so the RHI asks for
        // VK_KHR_display and nothing else; Vulkan builds a direct-to-display
        // surface from the physical device, not from the fd. fd is -1 until the
        // session claims a connector, which is what the target shipped with
        // before this was a variant alternative.
        auto surface = NativeSurfaceHandle(std::make_unique<NativeSurfaceHandle::Impl>(DrmTarget {.fd = -1, .connectorId = 0, .crtcId = 0}));
        target       = PresentationTarget::ForTTY(
            ttyContext, [](void* /*ctx*/) noexcept { /* the destructor restores text mode; see ~TTYBackend */ }, {.width = width, .height = height},
            std::move(surface)
        );
    }

    ~TTYBackend() {
        if (ttyContext != nullptr) {
            TTYBackend::Shutdown(ttyContext);
            ttyContext = nullptr;
        }
    }

    // A user-declared destructor suppresses the implicit move constructor and
    // leaves a copy constructor that would copy ttyContext -- two owners of one
    // lease, and Shutdown called on it twice. Move is written out and nulls the
    // source; copy is gone.
    TTYBackend(TTYBackend&& other) noexcept:
        target(std::move(other.target)), receiver(other.receiver), ttyContext(other.ttyContext), clipboard(std::move(other.clipboard)), closed(other.closed) {
        other.ttyContext = nullptr;
    }

    auto operator=(TTYBackend&& other) noexcept -> TTYBackend& {
        if (this != &other) {
            if (ttyContext != nullptr) {
                TTYBackend::Shutdown(ttyContext);
            }
            target           = std::move(other.target);
            receiver         = other.receiver;
            ttyContext       = other.ttyContext;
            clipboard        = std::move(other.clipboard);
            closed           = other.closed;
            other.ttyContext = nullptr;
        }
        return *this;
    }

    TTYBackend(const TTYBackend&)                    = delete;
    auto operator=(const TTYBackend&) -> TTYBackend& = delete;

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return ttyContext != nullptr;
    }
    [[nodiscard]] auto IsRunning() const noexcept -> bool {
        // Close() records the request; the terminal itself stays up until the
        // destructor restores text mode, which is what the crash handler's
        // EmergencyRestore also depends on.
        return !closed && ttyContext != nullptr && TTYBackend::IsRunning(ttyContext);
    }
    void PollEvents() noexcept {
        if (ttyContext != nullptr) {
            // The same WindowInputReceiver callbacks GLFW drives, so nothing
            // above this line can tell which source produced the event.
            TTYBackend::ProcessEvents(ttyContext, receiver);
        }
    }
    void Close() const noexcept {
        closed = true;
    }
};

// A desktop window. The only shape that touches a window system, and the only
// one whose AsWindow() is non-null.
struct WindowedBackend {
    std::unique_ptr<Window> window;

    explicit WindowedBackend(std::unique_ptr<Window> win) noexcept: window(std::move(win)) {
    }

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return window != nullptr && window->GetNativeHandle() != nullptr;
    }
    [[nodiscard]] auto IsRunning() const noexcept -> bool {
        return window != nullptr && window->IsRunning();
    }
    void PollEvents() noexcept {
        // The process-wide poll, not a per-window one: GLFW delivers every
        // window's events through the single queue, so the kernel polls once
        // and then asks each window what it saw.
        glfwPollEvents();
    }
    void Close() const noexcept {
        window->Close();
    }
};

} // namespace

struct PlatformHost::Impl {
    using BackendVariant = std::variant<std::monostate, HeadlessBackend, TTYBackend, WindowedBackend>;

    BackendVariant backend;

    // What the empty host hands back, so GetPresentationTarget() never has to
    // invent a reference. Nobody can draw into it: it is headless, zero-sized
    // and has no descriptor.
    PresentationTarget fallback = PresentationTarget::ForHeadless({.width = 0, .height = 0});
};

// --- Construction

PlatformHost::PlatformHost() noexcept: _impl(std::make_unique<Impl>()) {
}

PlatformHost::~PlatformHost() noexcept = default;

PlatformHost::PlatformHost(PlatformHost&& other) noexcept: _impl(std::move(other._impl)) {
    // Leaving the source empty rather than null, so a moved-from host is still
    // a host that answers -- every member is a no-op or a default.
    other._impl = std::make_unique<Impl>();
}

auto PlatformHost::operator=(PlatformHost&& other) noexcept -> PlatformHost& {
    if (this != &other) {
        _impl       = std::move(other._impl);
        other._impl = std::make_unique<Impl>();
    }
    return *this;
}

auto PlatformHost::CreateHeadless(uint32_t width, uint32_t height) -> PlatformHost {
    PlatformHost host;
    host._impl->backend = HeadlessBackend(width, height);
    ZHLN::Log("[Host] Headless session: no window, no event queue, no window system.");
    return host;
}

auto PlatformHost::CreateTTY(uint32_t width, uint32_t height, const WindowInputReceiver& receiver) -> PlatformHost {
    PlatformHost host;
    host._impl->backend = TTYBackend(width, height, receiver);
    if (!host.Valid()) {
        ZHLN::Log("[Host] TTY session failed: the terminal could not be taken over.");
        host._impl->backend = std::monostate {};
        return host;
    }
    ZHLN::Log("[Host] TTY session: direct to display over KMS/DRM, libevdev for input, no GLFW.");
    return host;
}

auto PlatformHost::CreateWindowed(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver)
    -> PlatformHost {
    PlatformHost host;

    auto window = std::make_unique<Window>(title, width, height, fullscreen, receiver);
    if (window->GetNativeHandle() == nullptr) {
        ZHLN::Log("[Host] Windowed session failed: the OS window could not be created.");
        return host;
    }
    host._impl->backend = WindowedBackend(std::move(window));
    return host;
}

auto PlatformHost::Valid() const noexcept -> bool {
    return std::visit(
        Overloaded {
            [](const std::monostate&) noexcept -> bool { return false; },
            [](const auto& backend) noexcept -> bool { return backend.Valid(); },
        },
        _impl->backend
    );
}

// --- Lifecycle

auto PlatformHost::IsRunning() const noexcept -> bool {
    return std::visit(
        Overloaded {
            [](const std::monostate&) noexcept -> bool { return false; },
            [](const auto& backend) noexcept -> bool { return backend.IsRunning(); },
        },
        _impl->backend
    );
}

void PlatformHost::PollEvents() noexcept {
    std::visit(
        Overloaded {
            [](std::monostate&) noexcept {},
            [](auto& backend) noexcept { backend.PollEvents(); },
        },
        _impl->backend
    );
}

void PlatformHost::Close() const noexcept {
    // _impl is a unique_ptr, so operator-> on a const host still yields a
    // mutable Impl: what a const Close() changes is run state behind the host,
    // which is exactly the constness it claims.
    std::visit(
        Overloaded {
            [](std::monostate&) noexcept {},
            [](auto& backend) noexcept { backend.Close(); },
        },
        _impl->backend
    );
}

// --- Presentation

auto PlatformHost::GetPresentationTarget() noexcept -> PresentationTarget& {
    return std::visit(
        Overloaded {
            [this](std::monostate&) noexcept -> PresentationTarget& { return _impl->fallback; },
            [](HeadlessBackend& backend) noexcept -> PresentationTarget& { return backend.target; },
            [](TTYBackend& backend) noexcept -> PresentationTarget& { return backend.target; },
            [](WindowedBackend& backend) noexcept -> PresentationTarget& { return backend.window->GetPresentationTarget(); },
        },
        _impl->backend
    );
}

auto PlatformHost::GetPresentationTarget() const noexcept -> const PresentationTarget& {
    return const_cast<PlatformHost*>(this)->GetPresentationTarget();
}

// --- Geometry

auto PlatformHost::GetSize() const noexcept -> Extent2D {
    return GetPresentationTarget().GetFramebufferExtent();
}

auto PlatformHost::HasNativeSurface() const noexcept -> bool {
    return GetPresentationTarget().GetNativeSurface().Valid();
}

// --- Desktop-only

void PlatformHost::Focus() noexcept {
    std::visit(
        Overloaded {
            [](std::monostate&) noexcept {},
            [](HeadlessBackend&) noexcept {},
            [](TTYBackend&) noexcept {},
            [](WindowedBackend& backend) noexcept { backend.window->Focus(); },
        },
        _impl->backend
    );
}

auto PlatformHost::IsFocused() const noexcept -> bool {
    return std::visit(
        Overloaded {
            // A session with no window has nothing to be unfocused; reporting false
            // here would make callers that gate on focus skip work they should do.
            [](const std::monostate&) noexcept -> bool { return true; },
            [](const HeadlessBackend&) noexcept -> bool { return true; },
            [](const TTYBackend&) noexcept -> bool { return true; },
            [](const WindowedBackend& backend) noexcept -> bool { return backend.window->IsFocused(); },
        },
        _impl->backend
    );
}

auto PlatformHost::WantsQuitProcess() const noexcept -> bool {
    return std::visit(
        Overloaded {
            [](const std::monostate&) noexcept -> bool { return false; },
            [](const HeadlessBackend&) noexcept -> bool { return false; },
            [](const TTYBackend&) noexcept -> bool { return false; },
            [](const WindowedBackend& backend) noexcept -> bool { return backend.window->WantsQuitProcess(); },
        },
        _impl->backend
    );
}

void PlatformHost::AcknowledgeQuitProcess() noexcept {
    std::visit(
        Overloaded {
            [](std::monostate&) noexcept {},
            [](HeadlessBackend&) noexcept {},
            [](TTYBackend&) noexcept {},
            [](WindowedBackend& backend) noexcept { backend.window->AcknowledgeQuitProcess(); },
        },
        _impl->backend
    );
}

auto PlatformHost::GetClipboardText() const -> std::string {
    return std::visit(
        Overloaded {
            [](const std::monostate&) -> std::string { return {}; },
            [](const HeadlessBackend& backend) -> std::string { return backend.clipboard; },
            [](const TTYBackend& backend) -> std::string { return backend.clipboard; },
            [](const WindowedBackend& backend) -> std::string { return backend.window->GetClipboardText(); },
        },
        _impl->backend
    );
}

void PlatformHost::SetClipboardText(std::string_view text) {
    std::visit(
        Overloaded {
            [](std::monostate&, std::string_view) {},
            [](HeadlessBackend& backend, std::string_view t) { backend.clipboard.assign(t); },
            [](TTYBackend& backend, std::string_view t) { backend.clipboard.assign(t); },
            [](WindowedBackend& backend, std::string_view t) { backend.window->SetClipboardText(t); },
        },
        _impl->backend, text
    );
}

void PlatformHost::SetFileDropHandler(void (*handler)(void* userdata, const FileDrop* files, uint32_t count), void* userdata) noexcept {
    std::visit(
        Overloaded {
            [](std::monostate&, auto, auto) noexcept {},
            [](HeadlessBackend&, auto, auto) noexcept {},
            [](TTYBackend&, auto, auto) noexcept {},
            [](WindowedBackend& backend, auto h, auto ud) noexcept { backend.window->SetFileDropHandler(h, ud); },
        },
        _impl->backend, handler, userdata
    );
}

// --- The window, without a downcast

auto PlatformHost::AsWindow() noexcept -> Window* {
    if (auto* backend = std::get_if<WindowedBackend>(&_impl->backend)) {
        return backend->window.get();
    }
    return nullptr;
}

auto PlatformHost::AsWindow() const noexcept -> const Window* {
    // A const host answers the question with a const window. Note this is not
    // reached through a const Impl -- _impl is a unique_ptr, whose operator-> is
    // itself const -- so the constness here is a promise to the caller, not a
    // restriction the compiler would have imposed.
    return const_cast<PlatformHost*>(this)->AsWindow();
}

} // namespace ZHLN
