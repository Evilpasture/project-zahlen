// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <type_traits>

namespace ZHLN {

class ConditionalVariable {
  public:
    constexpr ConditionalVariable() noexcept = default;
    ~ConditionalVariable()                   = default;

    ZHLN_API void Wait(Mutex& mutex) noexcept;

    ZHLN_API void NotifyOne() noexcept;

    ZHLN_API void NotifyAll() noexcept;

  private:
    ZHLN::Atomic<uint8_t> _bits {0};
};

static_assert(sizeof(ConditionalVariable) == 1, "ZHLN::ConditionalVariable must be exactly 1 byte!");
static_assert(std::is_standard_layout_v<ConditionalVariable>, "ZHLN::ConditionalVariable must be standard layout!");
static_assert(std::is_nothrow_default_constructible_v<ConditionalVariable>, "ZHLN::ConditionalVariable must be nothrow default constructible!");
static_assert(std::is_trivially_destructible_v<ConditionalVariable>, "ZHLN::ConditionalVariable must be trivially destructible!");
static_assert(std::is_trivially_copyable_v<ConditionalVariable>, "ZHLN::ConditionalVariable must be trivially copyable!");

}
