// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "NativeSurfaceInternal.hpp"
#include "PresentationTarget.hpp"

namespace ZHLN {

NativeSurfaceHandle::NativeSurfaceHandle() noexcept = default;

NativeSurfaceHandle::NativeSurfaceHandle(std::unique_ptr<Impl> impl) noexcept: _impl(std::move(impl)) {
}

NativeSurfaceHandle::~NativeSurfaceHandle() = default;

NativeSurfaceHandle::NativeSurfaceHandle(NativeSurfaceHandle&& other) noexcept = default;

auto NativeSurfaceHandle::operator=(NativeSurfaceHandle&& other) noexcept -> NativeSurfaceHandle& = default;

}
