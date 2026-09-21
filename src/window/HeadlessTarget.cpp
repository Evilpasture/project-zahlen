// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/window/HeadlessTarget.cpp

#include "HeadlessTarget.hpp"
#include "NativeSurfaceInternal.hpp"
#include <memory>

namespace ZHLN {

HeadlessPresentationTarget::HeadlessPresentationTarget(uint32_t width, uint32_t height) noexcept:
    _surface(std::make_unique<NativeSurfaceHandle::Impl>(HeadlessTarget {})), _extent {.width = width, .height = height} {
}

HeadlessPresentationTarget::~HeadlessPresentationTarget() = default;

auto HeadlessPresentationTarget::GetFramebufferExtent() const noexcept -> Extent2D {
    return _extent;
}

void HeadlessPresentationTarget::SetFramebufferExtent(uint32_t width, uint32_t height) noexcept {
    _extent = {.width = width, .height = height};
}

auto HeadlessPresentationTarget::GetNativeSurface() const noexcept -> const NativeSurfaceHandle& {
    return _surface;
}

auto HeadlessPresentationTarget::IsHeadless() const noexcept -> bool {
    return true;
}

auto HeadlessPresentationTarget::IsTTY() const noexcept -> bool {
    return false;
}

void HeadlessPresentationTarget::Close() const noexcept {
    _closed = true;
}

auto HeadlessPresentationTarget::WasClosed() const noexcept -> bool {
    return _closed;
}

} // namespace ZHLN
