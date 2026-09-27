// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Geometry2D.hpp>
#include <cstdint>
#include <memory>
#include <utility>

namespace ZHLN {

class NativeSurfaceHandle;

template <typename Visitor>
decltype(auto) Visit(const NativeSurfaceHandle& handle, Visitor&& visitor);

class ZHLN_API NativeSurfaceHandle {
  public:
    struct Impl;

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
    template <typename Visitor>
    friend decltype(auto) Visit(const NativeSurfaceHandle& handle, Visitor&& visitor);

    std::unique_ptr<Impl> _impl;
};

enum class TargetKind : uint8_t {
    Window,
    TTY,
    Headless,
};

class ZHLN_API PresentationTarget {
  public:
    using ExtentFn = Extent2D (*)(void* userdata) noexcept;
    using CloseFn  = void (*)(void* userdata) noexcept;

    PresentationTarget() noexcept = default;
    ~PresentationTarget()         = default;

    PresentationTarget(PresentationTarget&& other) noexcept;
    auto operator=(PresentationTarget&& other) noexcept -> PresentationTarget&;

    PresentationTarget(const PresentationTarget&)                    = delete;
    auto operator=(const PresentationTarget&) -> PresentationTarget& = delete;

    [[nodiscard]] static auto ForWindow(void* userdata, ExtentFn extentFn, CloseFn closeFn, NativeSurfaceHandle surface = {}) noexcept -> PresentationTarget {
        return PresentationTarget(TargetKind::Window, userdata, extentFn, closeFn, std::move(surface), {});
    }

    [[nodiscard]] static auto ForHeadless(Extent2D extent, NativeSurfaceHandle surface = {}) noexcept -> PresentationTarget {
        return PresentationTarget(TargetKind::Headless, nullptr, nullptr, nullptr, std::move(surface), extent);
    }

    [[nodiscard]] static auto ForTTY(void* userdata, CloseFn closeFn, Extent2D extent, NativeSurfaceHandle surface) noexcept -> PresentationTarget {
        return PresentationTarget(TargetKind::TTY, userdata, nullptr, closeFn, std::move(surface), extent);
    }

    [[nodiscard]] auto GetFramebufferExtent() const noexcept -> Extent2D {
        return _extentFn != nullptr ? _extentFn(_userdata) : _staticExtent;
    }

    void SetFramebufferExtent(uint32_t width, uint32_t height) noexcept {
        _staticExtent = {.width = width, .height = height};
    }

    [[nodiscard]] auto GetNativeSurface() const noexcept -> const NativeSurfaceHandle& {
        return _surface;
    }

    void SetNativeSurface(NativeSurfaceHandle surface) noexcept {
        _surface = std::move(surface);
    }

    [[nodiscard]] auto Kind() const noexcept -> TargetKind {
        return _kind;
    }

    [[nodiscard]] auto IsHeadless() const noexcept -> bool {
        return _kind == TargetKind::Headless;
    }

    [[nodiscard]] auto IsTTY() const noexcept -> bool {
        return _kind == TargetKind::TTY;
    }

    void Close() const noexcept {
        if (_closeFn != nullptr) {
            _closeFn(_userdata);
        }
        _closed = true;
    }

    [[nodiscard]] auto WasClosed() const noexcept -> bool {
        return _closed;
    }

  private:
    PresentationTarget(TargetKind kind, void* userdata, ExtentFn extentFn, CloseFn closeFn, NativeSurfaceHandle surface, Extent2D extent) noexcept:
        _surface(std::move(surface)), _staticExtent(extent), _userdata(userdata), _extentFn(extentFn), _closeFn(closeFn), _kind(kind) {
    }

    NativeSurfaceHandle _surface;
    Extent2D            _staticExtent {.width = 0, .height = 0};
    void*               _userdata = nullptr;
    ExtentFn            _extentFn = nullptr;
    CloseFn             _closeFn  = nullptr;
    TargetKind          _kind     = TargetKind::Headless;
    mutable bool _closed = false;
};

}
