// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/window/PlatformHost.cpp

#include "PlatformHostInternal.hpp"
#include "tty/TTYBackend.hpp"
#include <GLFW/glfw3.h>
#include <Zahlen/Log.hpp>

namespace ZHLN {

// --- IPlatformTarget defaults
//
// The vtable anchor. Defining the destructor here is what makes it exist once
// instead of weakly in every translation unit that includes the header.

IPlatformHost::~IPlatformHost() = default;

void IPlatformHost::Focus() noexcept {
}

auto IPlatformHost::IsFocused() const noexcept -> bool {
    // A session with no window has nothing to be unfocused; reporting false
    // here would make callers that gate on focus skip work they should do.
    return true;
}

auto IPlatformHost::WantsQuitProcess() const noexcept -> bool {
    return false;
}

void IPlatformHost::AcknowledgeQuitProcess() noexcept {
}

auto IPlatformHost::GetClipboardText() const -> std::string {
    return {};
}

void IPlatformHost::SetClipboardText(std::string_view text) {
    static_cast<void>(text);
}

void IPlatformHost::SetFileDropHandler(void (*handler)(void* userdata, const FileDrop* files, uint32_t count), void* userdata) noexcept {
    static_cast<void>(handler);
    static_cast<void>(userdata);
}

auto IPlatformHost::AsWindow() noexcept -> Window* {
    return nullptr;
}

// --- HeadlessPlatformHost

HeadlessPlatformHost::HeadlessPlatformHost(uint32_t width, uint32_t height) noexcept: _target(width, height) {
}

HeadlessPlatformHost::~HeadlessPlatformHost() = default;

auto HeadlessPlatformHost::IsRunning() const noexcept -> bool {
    return _running;
}

void HeadlessPlatformHost::PollEvents() noexcept {
    // No event source. Deliberately not a call into GLFW: a headless session
    // never initialised it.
}

void HeadlessPlatformHost::Close() const noexcept {
    _running = false;
}

auto HeadlessPlatformHost::GetPresentationTarget() noexcept -> IPresentationTarget& {
    return _target;
}

auto HeadlessPlatformHost::GetPresentationTarget() const noexcept -> const IPresentationTarget& {
    return _target;
}

auto HeadlessPlatformHost::GetSize() const noexcept -> Extent2D {
    return _target.GetFramebufferExtent();
}

auto HeadlessPlatformHost::HasNativeSurface() const noexcept -> bool {
    return _target.GetNativeSurface().Valid();
}

auto HeadlessPlatformHost::GetClipboardText() const -> std::string {
    return _clipboard;
}

void HeadlessPlatformHost::SetClipboardText(std::string_view text) {
    _clipboard.assign(text);
}

// --- TTYPlatformHost

TTYPlatformHost::TTYPlatformHost(uint32_t width, uint32_t height, const WindowInputReceiver& receiver) noexcept:
    _target(-1, 0, 0, width, height), _receiver(receiver) {
    _ttyContext = TTYBackend::Init(width, height);
}

TTYPlatformHost::~TTYPlatformHost() {
    if (_ttyContext != nullptr) {
        TTYBackend::Shutdown(_ttyContext);
        _ttyContext = nullptr;
    }
}

auto TTYPlatformHost::Valid() const noexcept -> bool {
    return _ttyContext != nullptr;
}

auto TTYPlatformHost::IsRunning() const noexcept -> bool {
    // Close() records the request; the terminal itself stays up until the
    // destructor restores text mode, which is what the crash handler's
    // EmergencyRestore also depends on.
    return !_closed && _ttyContext != nullptr && TTYBackend::IsRunning(_ttyContext);
}

void TTYPlatformHost::PollEvents() noexcept {
    if (_ttyContext != nullptr) {
        // The same WindowInputReceiver callbacks GLFW drives, so nothing above
        // this line can tell which source produced the event.
        TTYBackend::ProcessEvents(_ttyContext, _receiver);
    }
}

void TTYPlatformHost::Close() const noexcept {
    _closed = true;
}

auto TTYPlatformHost::GetPresentationTarget() noexcept -> IPresentationTarget& {
    return _target;
}

auto TTYPlatformHost::GetPresentationTarget() const noexcept -> const IPresentationTarget& {
    return _target;
}

auto TTYPlatformHost::GetSize() const noexcept -> Extent2D {
    return _target.GetFramebufferExtent();
}

auto TTYPlatformHost::HasNativeSurface() const noexcept -> bool {
    return _target.GetNativeSurface().Valid();
}

auto TTYPlatformHost::GetClipboardText() const -> std::string {
    return _clipboard;
}

void TTYPlatformHost::SetClipboardText(std::string_view text) {
    _clipboard.assign(text);
}

// --- WindowedPlatformHost

WindowedPlatformHost::WindowedPlatformHost(std::unique_ptr<Window> window) noexcept: _window(std::move(window)) {
}

WindowedPlatformHost::~WindowedPlatformHost() = default;

auto WindowedPlatformHost::Valid() const noexcept -> bool {
    return _window != nullptr && _window->GetNativeHandle() != nullptr;
}

auto WindowedPlatformHost::IsRunning() const noexcept -> bool {
    return _window->IsRunning();
}

void WindowedPlatformHost::PollEvents() noexcept {
    // The process-wide poll, not a per-window one: GLFW delivers every window's
    // events through the single queue, so the kernel polls once and then asks
    // each window what it saw.
    glfwPollEvents();
}

void WindowedPlatformHost::Close() const noexcept {
    _window->Close();
}

auto WindowedPlatformHost::GetPresentationTarget() noexcept -> IPresentationTarget& {
    return _window->GetPresentationTarget();
}

auto WindowedPlatformHost::GetPresentationTarget() const noexcept -> const IPresentationTarget& {
    return _window->GetPresentationTarget();
}

auto WindowedPlatformHost::GetSize() const noexcept -> Extent2D {
    return _window->GetSize();
}

auto WindowedPlatformHost::HasNativeSurface() const noexcept -> bool {
    return _window->GetPresentationTarget().GetNativeSurface().Valid();
}

void WindowedPlatformHost::Focus() noexcept {
    _window->Focus();
}

auto WindowedPlatformHost::IsFocused() const noexcept -> bool {
    return _window->IsFocused();
}

auto WindowedPlatformHost::WantsQuitProcess() const noexcept -> bool {
    return _window->WantsQuitProcess();
}

void WindowedPlatformHost::AcknowledgeQuitProcess() noexcept {
    _window->AcknowledgeQuitProcess();
}

auto WindowedPlatformHost::GetClipboardText() const -> std::string {
    return _window->GetClipboardText();
}

void WindowedPlatformHost::SetClipboardText(std::string_view text) {
    _window->SetClipboardText(text);
}

void WindowedPlatformHost::SetFileDropHandler(void (*handler)(void* userdata, const FileDrop* files, uint32_t count), void* userdata) noexcept {
    _window->SetFileDropHandler(handler, userdata);
}

auto WindowedPlatformHost::AsWindow() noexcept -> Window* {
    return _window.get();
}

// --- Factories

auto CreateHeadlessHost(uint32_t width, uint32_t height) -> std::unique_ptr<IPlatformHost> {
    ZHLN::Log("[Host] Headless session: no window, no event queue, no window system.");
    return std::make_unique<HeadlessPlatformHost>(width, height);
}

auto CreateTTYHost(uint32_t width, uint32_t height, const WindowInputReceiver& receiver) -> std::unique_ptr<IPlatformHost> {
    auto host = std::make_unique<TTYPlatformHost>(width, height, receiver);
    if (!host->Valid()) {
        ZHLN::Log("[Host] TTY session failed: the terminal could not be taken over.");
        return nullptr;
    }
    ZHLN::Log("[Host] TTY session: direct to display over KMS/DRM, libevdev for input, no GLFW.");
    return host;
}

auto CreateWindowedHost(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver)
    -> std::unique_ptr<IPlatformHost> {
    auto window = std::make_unique<Window>(title, width, height, fullscreen, receiver);
    if (window->GetNativeHandle() == nullptr) {
        ZHLN::Log("[Host] Windowed session failed: the OS window could not be created.");
        return nullptr;
    }
    return std::make_unique<WindowedPlatformHost>(std::move(window));
}

} // namespace ZHLN
