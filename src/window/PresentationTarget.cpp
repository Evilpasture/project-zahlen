// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/window/PresentationTarget.cpp

#include "PresentationTarget.hpp"
#include <utility>

namespace ZHLN {

// Out of line so the move is defined once, next to the handle it moves, rather
// than being instantiated into every translation unit that installs a target.
PresentationTarget::PresentationTarget(PresentationTarget&& other) noexcept:
    _surface(std::move(other._surface)), _staticExtent(other._staticExtent), _userdata(other._userdata), _extentFn(other._extentFn), _closeFn(other._closeFn),
    _kind(other._kind), _closed(other._closed) {
    // A moved-from target is the empty one: Headless, no producer, no
    // descriptor. Not left as a copy of the source, which would leave two
    // targets claiming the same userdata and the same run state.
    other._userdata = nullptr;
    other._extentFn = nullptr;
    other._closeFn  = nullptr;
    other._kind     = TargetKind::Headless;
    other._closed   = false;
}

auto PresentationTarget::operator=(PresentationTarget&& other) noexcept -> PresentationTarget& {
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

} // namespace ZHLN
