// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "PresentationTarget.hpp"
#include "tty/TTYBackend.hpp"
#include <GLFW/glfw3.h>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Window/PlatformHost.hpp>
#include <Zahlen/Window/Window.hpp>
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <variant>

namespace ZHLN {


namespace {

struct HeadlessBackend {
    PresentationTarget target;
    std::string        clipboard;
    CursorMode         cursorMode    = CursorMode::System;
    bool               mouseCaptured = false;

    HeadlessBackend(uint32_t width, uint32_t height) noexcept: target(PresentationTarget::ForHeadless({.width = width, .height = height})) {
    }

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return true;
    }
    [[nodiscard]] auto IsRunning() const noexcept -> bool {
        return !target.WasClosed();
    }
    void PollEvents() noexcept {
    }
    void Close() const noexcept {
        target.Close();
    }
    void SetCursorMode(CursorMode mode) noexcept {
        cursorMode = mode;
    }
    [[nodiscard]] auto GetCursorMode() const noexcept -> CursorMode {
        return cursorMode;
    }
    void CaptureMouse(bool captured) noexcept {
        mouseCaptured = captured;
    }
    [[nodiscard]] auto IsMouseCaptured() const noexcept -> bool {
        return mouseCaptured;
    }
};

struct TTYBackendState {
    PresentationTarget  target;
    WindowInputReceiver receiver;
    void*               ttyContext = nullptr;
    std::string         clipboard;
    CursorMode          cursorMode = CursorMode::Custom;

    TTYBackendState(uint32_t width, uint32_t height, const WindowInputReceiver& rx) noexcept: receiver(rx) {
        ttyContext = TTYBackend::Init(width, height);

        auto surface = NativeSurfaceHandle(DrmTarget {.fd = -1, .connectorId = 0, .crtcId = 0});
        target       = PresentationTarget::ForTTY(
            ttyContext, [](void* ) noexcept {  }, {.width = width, .height = height},
            std::move(surface)
        );
    }

    ~TTYBackendState() {
        if (ttyContext != nullptr) {
            TTYBackend::Shutdown(ttyContext);
            ttyContext = nullptr;
        }
    }

    TTYBackendState(TTYBackendState&& other) noexcept:
        target(std::move(other.target)), receiver(other.receiver), ttyContext(other.ttyContext), clipboard(std::move(other.clipboard)),
        cursorMode(other.cursorMode) {
        other.ttyContext = nullptr;
    }

    auto operator=(TTYBackendState&& other) noexcept -> TTYBackendState& {
        if (this != &other) {
            if (ttyContext != nullptr) {
                TTYBackend::Shutdown(ttyContext);
            }
            target           = std::move(other.target);
            receiver         = other.receiver;
            ttyContext       = other.ttyContext;
            clipboard        = std::move(other.clipboard);
            cursorMode       = other.cursorMode;
            other.ttyContext = nullptr;
        }
        return *this;
    }

    TTYBackendState(const TTYBackendState&)                    = delete;
    auto operator=(const TTYBackendState&) -> TTYBackendState& = delete;

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return ttyContext != nullptr;
    }
    [[nodiscard]] auto IsRunning() const noexcept -> bool {
        return !target.WasClosed() && ttyContext != nullptr && TTYBackend::IsRunning(ttyContext);
    }
    void PollEvents() noexcept {
        if (ttyContext != nullptr) {
            const Extent2D extent = target.GetFramebufferExtent();
            TTYBackend::SetScreenSize(ttyContext, extent.width, extent.height);
            TTYBackend::ProcessEvents(ttyContext, receiver);
        }
    }
    void Close() const noexcept {
        target.Close();
    }
    void SetCursorMode(CursorMode mode) noexcept {
        cursorMode = mode;
    }
    [[nodiscard]] auto GetCursorMode() const noexcept -> CursorMode {
        return cursorMode;
    }
    void CaptureMouse(bool captured) noexcept {
        if (ttyContext != nullptr) {
            TTYBackend::SetMouseCaptured(ttyContext, captured);
        }
    }
    [[nodiscard]] auto IsMouseCaptured() const noexcept -> bool {
        return ttyContext != nullptr && TTYBackend::IsMouseCaptured(ttyContext);
    }
};

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
        glfwPollEvents();
        if (window != nullptr) {
            window->ProcessEvents();
        }
    }
    void Close() const noexcept {
        window->Close();
    }
    void SetCursorMode(CursorMode mode) noexcept {
        if (window != nullptr) {
            window->SetCursorMode(mode);
        }
    }
    [[nodiscard]] auto GetCursorMode() const noexcept -> CursorMode {
        return window != nullptr ? window->GetCursorMode() : CursorMode::System;
    }
    void CaptureMouse(bool captured) noexcept {
        if (window != nullptr) {
            window->CaptureMouse(captured);
        }
    }
    [[nodiscard]] auto IsMouseCaptured() const noexcept -> bool {
        return window != nullptr && window->IsMouseCaptured();
    }
};

}

struct PlatformHost::Impl {
    using BackendVariant = std::variant<std::monostate, HeadlessBackend, TTYBackendState, WindowedBackend>;

    BackendVariant backend;

    PresentationTarget fallback = PresentationTarget::ForHeadless({.width = 0, .height = 0});
};


PlatformHost::PlatformHost() noexcept: _impl(std::make_unique<Impl>()) {
}

PlatformHost::~PlatformHost() noexcept = default;

PlatformHost::PlatformHost(PlatformHost&& other) noexcept: _impl(std::move(other._impl)) {
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
    host._impl->backend = TTYBackendState(width, height, receiver);
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
    std::visit(
        Overloaded {
            [](std::monostate&) noexcept {},
            [](auto& backend) noexcept { backend.Close(); },
        },
        _impl->backend
    );
}


auto PlatformHost::Target() noexcept -> PresentationTarget& {
    return std::visit(
        Overloaded {
            [this](std::monostate&) noexcept -> PresentationTarget& { return _impl->fallback; },
            [](HeadlessBackend& backend) noexcept -> PresentationTarget& { return backend.target; },
            [](TTYBackendState& backend) noexcept -> PresentationTarget& { return backend.target; },
            [](WindowedBackend& backend) noexcept -> PresentationTarget& { return backend.window->Target(); },
        },
        _impl->backend
    );
}

auto PlatformHost::Target() const noexcept -> const PresentationTarget& {
    return const_cast<PlatformHost*>(this)->Target();
}

auto PlatformHost::TargetFor(Window& window) noexcept -> PresentationTarget& {
    return window.Target();
}


auto PlatformHost::GetSize() const noexcept -> Extent2D {
    return Target().GetFramebufferExtent();
}

auto PlatformHost::HasNativeSurface() const noexcept -> bool {
    return Target().GetNativeSurface().Valid();
}

void PlatformHost::SetCursorMode(CursorMode mode) noexcept {
    std::visit(
        Overloaded {
            [](std::monostate&) noexcept {},
            [mode](auto& backend) noexcept { backend.SetCursorMode(mode); },
        },
        _impl->backend
    );
}

auto PlatformHost::GetCursorMode() const noexcept -> CursorMode {
    return std::visit(
        Overloaded {
            [](const std::monostate&) noexcept -> CursorMode { return CursorMode::System; },
            [](const auto& backend) noexcept -> CursorMode { return backend.GetCursorMode(); },
        },
        _impl->backend
    );
}

auto PlatformHost::UsesCustomCursor() const noexcept -> bool {
    return Valid() && GetCursorMode() == CursorMode::Custom && !IsMouseCaptured();
}

void PlatformHost::CaptureMouse(bool captured) noexcept {
    std::visit(
        Overloaded {
            [](std::monostate&) noexcept {},
            [captured](auto& backend) noexcept { backend.CaptureMouse(captured); },
        },
        _impl->backend
    );
}

auto PlatformHost::IsMouseCaptured() const noexcept -> bool {
    return std::visit(
        Overloaded {
            [](const std::monostate&) noexcept -> bool { return false; },
            [](const auto& backend) noexcept -> bool { return backend.IsMouseCaptured(); },
        },
        _impl->backend
    );
}


void PlatformHost::Focus() noexcept {
    std::visit(
        Overloaded {
            [](std::monostate&) noexcept {},
            [](HeadlessBackend&) noexcept {},
            [](TTYBackendState&) noexcept {},
            [](WindowedBackend& backend) noexcept { backend.window->Focus(); },
        },
        _impl->backend
    );
}

auto PlatformHost::IsFocused() const noexcept -> bool {
    return std::visit(
        Overloaded {
            [](const std::monostate&) noexcept -> bool { return true; },
            [](const HeadlessBackend&) noexcept -> bool { return true; },
            [](const TTYBackendState&) noexcept -> bool { return true; },
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
            [](const TTYBackendState&) noexcept -> bool { return false; },
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
            [](TTYBackendState&) noexcept {},
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
            [](const TTYBackendState& backend) -> std::string { return backend.clipboard; },
            [](const WindowedBackend& backend) -> std::string { return backend.window->GetClipboardText(); },
        },
        _impl->backend
    );
}

void PlatformHost::SetClipboardText(std::string_view text) {
    std::visit(
        Overloaded {
            [](std::monostate&) {},
            [text](HeadlessBackend& backend) { backend.clipboard.assign(text); },
            [text](TTYBackendState& backend) { backend.clipboard.assign(text); },
            [text](WindowedBackend& backend) { backend.window->SetClipboardText(text); },
        },
        _impl->backend
    );
}

void PlatformHost::SetFileDropHandler(void (*handler)(void* userdata, const FileDrop* files, uint32_t count), void* userdata) noexcept {
    std::visit(
        Overloaded {
            [](std::monostate&) noexcept {},
            [](HeadlessBackend&) noexcept {},
            [](TTYBackendState&) noexcept {},
            [handler, userdata](WindowedBackend& backend) noexcept { backend.window->SetFileDropHandler(handler, userdata); },
        },
        _impl->backend
    );
}

auto PlatformHost::SetGamepadRumble(uint8_t slot, [[maybe_unused]] const GamepadRumble& rumble) noexcept -> bool {
    if (slot >= kMaxGamepads) {
        return false;
    }
#if defined(_WIN32)
    struct XInputVibration {
        uint16_t wLeftMotorSpeed;
        uint16_t wRightMotorSpeed;
    };
    using XInputSetStateFn = unsigned long(__stdcall*)(unsigned long, XInputVibration*);

    static XInputSetStateFn s_xinputSetState = []() noexcept -> XInputSetStateFn {
        HMODULE mod = LoadLibraryW(L"xinput1_4.dll");
        if (mod == nullptr) {
            mod = LoadLibraryW(L"xinput9_1_0.dll");
        }
        if (mod == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<XInputSetStateFn>(GetProcAddress(mod, "XInputSetState"));
    }();

    if (s_xinputSetState != nullptr) {
        XInputVibration vib {
            .wLeftMotorSpeed  = static_cast<uint16_t>(std::clamp(rumble.lowFrequency, 0.0f, 1.0f) * 65535.0f),
            .wRightMotorSpeed = static_cast<uint16_t>(std::clamp(rumble.highFrequency, 0.0f, 1.0f) * 65535.0f),
        };
        return s_xinputSetState(static_cast<unsigned long>(slot), &vib) == 0UL;
    }
#endif
    return false;
}


auto PlatformHost::AsWindow() noexcept -> ZHLN::Optional<Window&> {
    if (auto* backend = std::get_if<WindowedBackend>(&_impl->backend)) {
        if (backend->window != nullptr) {
            return *backend->window;
        }
    }
    return std::nullopt;
}

auto PlatformHost::AsWindow() const noexcept -> ZHLN::Optional<const Window&> {
    if (const auto* backend = std::get_if<WindowedBackend>(&_impl->backend)) {
        if (backend->window != nullptr) {
            return *backend->window;
        }
    }
    return std::nullopt;
}

}
