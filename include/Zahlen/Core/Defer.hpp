// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <type_traits>
#include <utility>

namespace ZHLN {

// A lexical, allocation-free cleanup action. C++26's unnamed placeholder lets
// call sites spell this as `defer _([&] { ... });` without inventing a name.
template <typename F>
class defer {
  public:
    explicit defer(F fn) noexcept(std::is_nothrow_move_constructible_v<F>): _fn(std::move(fn)) {}
    ~defer() noexcept {
        if (_active) {
            _fn();
        }
    }

    defer(const defer&)                    = delete;
    auto operator=(const defer&) -> defer& = delete;

    defer(defer&& other) noexcept(std::is_nothrow_move_constructible_v<F>):
        _fn(std::move(other._fn)), _active(std::exchange(other._active, false)) {}
    auto operator=(defer&&) -> defer& = delete;

    void Dismiss() noexcept { _active = false; }

  private:
    F    _fn;
    bool _active = true;
};

template <typename F>
defer(F) -> defer<F>;

} // namespace ZHLN
