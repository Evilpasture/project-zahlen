// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Geometry2D.hpp>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace ZHLN {

struct Win32Target {
    void* hwnd      = nullptr;
    void* hinstance = nullptr;
};

struct WaylandTarget {
    void* display = nullptr;
    void* surface = nullptr;
};

struct X11Target {
    void*         display = nullptr;
    unsigned long window  = 0;
};

struct CocoaTarget {
    void* caMetalLayer = nullptr;
};

struct DrmTarget {
    int      fd          = -1;
    uint32_t connectorId = 0;
    uint32_t crtcId      = 0;
};

struct HeadlessTarget {};

using NativeSurfaceVariant = std::variant<HeadlessTarget, Win32Target, WaylandTarget, X11Target, CocoaTarget, DrmTarget>;

template <class... Ts>
struct Overloaded: Ts... {
    using Ts::operator()...;
};

class NativeSurfaceHandle;

template <typename Visitor>
constexpr decltype(auto) Visit(const NativeSurfaceHandle& handle, Visitor&& visitor);

class NativeSurfaceHandle {
  public:
    constexpr NativeSurfaceHandle() noexcept = default;

    explicit constexpr NativeSurfaceHandle(NativeSurfaceVariant target) noexcept: _target(target) {
    }

    template <typename T>
        requires(!std::is_same_v<std::remove_cvref_t<T>, NativeSurfaceHandle> &&
                 !std::is_same_v<std::remove_cvref_t<T>, NativeSurfaceVariant> &&
                 std::is_constructible_v<NativeSurfaceVariant, T>)
    explicit constexpr NativeSurfaceHandle(T&& target) noexcept: _target(std::in_place, std::forward<T>(target)) {
    }

    constexpr ~NativeSurfaceHandle() = default;

    constexpr NativeSurfaceHandle(NativeSurfaceHandle&& other) noexcept: _target(std::exchange(other._target, std::nullopt)) {
    }

    constexpr auto operator=(NativeSurfaceHandle&& other) noexcept -> NativeSurfaceHandle& {
        if (this != &other) {
            _target = std::exchange(other._target, std::nullopt);
        }
        return *this;
    }

    NativeSurfaceHandle(const NativeSurfaceHandle&)                    = delete;
    auto operator=(const NativeSurfaceHandle&) -> NativeSurfaceHandle& = delete;

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return _target.has_value();
    }

  private:
    template <typename Visitor>
    friend constexpr decltype(auto) Visit(const NativeSurfaceHandle& handle, Visitor&& visitor);

    std::optional<NativeSurfaceVariant> _target;
};

template <typename Visitor>
constexpr decltype(auto) Visit(const NativeSurfaceHandle& handle, Visitor&& visitor) {
    return std::visit(std::forward<Visitor>(visitor), *handle._target);
}

enum class TargetKind : uint8_t {
    Window,
    TTY,
    Headless,
};

class PresentationTarget {
  public:
    using ExtentFn = Extent2D (*)(void* userdata) noexcept;
    using CloseFn  = void (*)(void* userdata) noexcept;

    constexpr PresentationTarget() noexcept  = default;
    constexpr ~PresentationTarget() noexcept = default;

    constexpr PresentationTarget(PresentationTarget&& other) noexcept:
        _surface(std::move(other._surface)), _staticExtent(other._staticExtent), _userdata(other._userdata), _extentFn(other._extentFn),
        _closeFn(other._closeFn), _kind(other._kind), _closed(other._closed) {
        other._userdata = nullptr;
        other._extentFn = nullptr;
        other._closeFn  = nullptr;
        other._kind     = TargetKind::Headless;
        other._closed   = false;
    }

    constexpr auto operator=(PresentationTarget&& other) noexcept -> PresentationTarget& {
        if (this != &other) {
            _surface      = std::move(other._surface);
            _staticExtent = other._staticExtent;
            _userdata     = other._userdata;
            _extentFn     = other._extentFn;
            _closeFn      = other._closeFn;
            _kind         = other._kind;
            _closed       = other._closed;

            other._userdata = nullptr;
            other._extentFn = nullptr;
            other._closeFn  = nullptr;
            other._kind     = TargetKind::Headless;
            other._closed   = false;
        }
        return *this;
    }

    PresentationTarget(const PresentationTarget&)                    = delete;
    auto operator=(const PresentationTarget&) -> PresentationTarget& = delete;

    [[nodiscard]] static constexpr auto
        ForWindow(void* userdata, ExtentFn extentFn, CloseFn closeFn, NativeSurfaceHandle surface = {}) noexcept -> PresentationTarget {
        return PresentationTarget(TargetKind::Window, userdata, extentFn, closeFn, std::move(surface), {});
    }

    [[nodiscard]] static constexpr auto ForHeadless(Extent2D extent, NativeSurfaceHandle surface = {}) noexcept -> PresentationTarget {
        return PresentationTarget(TargetKind::Headless, nullptr, nullptr, nullptr, std::move(surface), extent);
    }

    [[nodiscard]] static constexpr auto
        ForTTY(void* userdata, CloseFn closeFn, Extent2D extent, NativeSurfaceHandle surface) noexcept -> PresentationTarget {
        return PresentationTarget(TargetKind::TTY, userdata, nullptr, closeFn, std::move(surface), extent);
    }

    [[nodiscard]] auto GetFramebufferExtent() const noexcept -> Extent2D {
        return _extentFn != nullptr ? _extentFn(_userdata) : _staticExtent;
    }

    constexpr void SetFramebufferExtent(uint32_t width, uint32_t height) noexcept {
        _staticExtent = {.width = width, .height = height};
    }

    [[nodiscard]] constexpr auto GetNativeSurface() const noexcept -> const NativeSurfaceHandle& {
        return _surface;
    }

    constexpr void SetNativeSurface(NativeSurfaceHandle surface) noexcept {
        _surface = std::move(surface);
    }

    [[nodiscard]] constexpr auto Kind() const noexcept -> TargetKind {
        return _kind;
    }

    [[nodiscard]] constexpr auto IsHeadless() const noexcept -> bool {
        return _kind == TargetKind::Headless;
    }

    [[nodiscard]] constexpr auto IsTTY() const noexcept -> bool {
        return _kind == TargetKind::TTY;
    }

    void Close() const noexcept {
        if (_closeFn != nullptr) {
            _closeFn(_userdata);
        }
        _closed = true;
    }

    [[nodiscard]] constexpr auto WasClosed() const noexcept -> bool {
        return _closed;
    }

  private:
    constexpr PresentationTarget(
        TargetKind kind, void* userdata, ExtentFn extentFn, CloseFn closeFn, NativeSurfaceHandle surface, Extent2D extent
    ) noexcept:
        _surface(std::move(surface)), _staticExtent(extent), _userdata(userdata), _extentFn(extentFn), _closeFn(closeFn), _kind(kind) {
    }

    NativeSurfaceHandle _surface;
    Extent2D            _staticExtent {.width = 0, .height = 0};
    void*               _userdata = nullptr;
    ExtentFn            _extentFn = nullptr;
    CloseFn             _closeFn  = nullptr;
    TargetKind          _kind     = TargetKind::Headless;
    mutable bool        _closed   = false;
};

} // namespace ZHLN
