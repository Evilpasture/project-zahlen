// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/window/NativeSurfaceHandle.cpp
//
// The PIMPL lifetime, out of line: this is the translation unit that knows
// NativeSurfaceHandle::Impl is a variant of OS descriptors, so it is also the
// one that can destroy one. Nothing outside src/window/ and src/vulkan/ needs
// that, and nothing outside them includes the header that says so.

#include "NativeSurfaceInternal.hpp"
#include <Zahlen/PresentationTarget.hpp>

namespace ZHLN {

NativeSurfaceHandle::NativeSurfaceHandle() noexcept = default;

NativeSurfaceHandle::NativeSurfaceHandle(std::unique_ptr<Impl> impl) noexcept: _impl(std::move(impl)) {
}

NativeSurfaceHandle::~NativeSurfaceHandle() = default;

NativeSurfaceHandle::NativeSurfaceHandle(NativeSurfaceHandle&& other) noexcept = default;

auto NativeSurfaceHandle::operator=(NativeSurfaceHandle&& other) noexcept -> NativeSurfaceHandle& = default;

auto NativeSurfaceHandle::GetImpl() const noexcept -> const Impl& {
    // Callers reach this through Visit(), which is only reachable for a handle
    // the window side built; Valid() is the guard a consumer checks first.
    return *_impl;
}

} // namespace ZHLN
