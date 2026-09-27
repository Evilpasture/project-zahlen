// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "PresentationTarget.hpp"
#include <utility>

namespace ZHLN {

PresentationTarget::PresentationTarget(PresentationTarget&& other) noexcept:
    _surface(std::move(other._surface)), _staticExtent(other._staticExtent), _userdata(other._userdata), _extentFn(other._extentFn), _closeFn(other._closeFn),
    _kind(other._kind), _closed(other._closed) {
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

}
