// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/window/DrmTarget.cpp

#include "DrmTarget.hpp"
#include "NativeSurfaceInternal.hpp"
#include <memory>

namespace ZHLN {

DrmPresentationTarget::DrmPresentationTarget(int fd, uint32_t connectorId, uint32_t crtcId, uint32_t width, uint32_t height) noexcept:
    _surface(std::make_unique<NativeSurfaceHandle::Impl>(DrmTarget {.fd = fd, .connectorId = connectorId, .crtcId = crtcId})),
    _extent {.width = width, .height = height}, _fd(fd) {
}

DrmPresentationTarget::~DrmPresentationTarget() = default;

auto DrmPresentationTarget::GetFramebufferExtent() const noexcept -> Extent2D {
    return _extent;
}

void DrmPresentationTarget::SetFramebufferExtent(uint32_t width, uint32_t height) noexcept {
    _extent = {.width = width, .height = height};
}

auto DrmPresentationTarget::GetNativeSurface() const noexcept -> const NativeSurfaceHandle& {
    return _surface;
}

auto DrmPresentationTarget::IsHeadless() const noexcept -> bool {
    return false;
}

auto DrmPresentationTarget::IsTTY() const noexcept -> bool {
    return true;
}

void DrmPresentationTarget::Close() const noexcept {
    _closed = true;
}

auto DrmPresentationTarget::WasClosed() const noexcept -> bool {
    return _closed;
}

auto DrmPresentationTarget::Fd() const noexcept -> int {
    return _fd;
}

} // namespace ZHLN
