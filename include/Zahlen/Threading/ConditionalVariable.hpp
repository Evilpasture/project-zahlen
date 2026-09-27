// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <type_traits>

namespace ZHLN {

class ConditionalVariable {
  public:

    void Wait(Mutex& mutex) noexcept;

    void NotifyOne() noexcept;

    void NotifyAll() noexcept;

  private:
    ZHLN::Atomic<uint8_t> _bits;
};

static_assert(sizeof(ConditionalVariable) == 1, "ZHLN::ConditionalVariable must be exactly 1 byte!");
static_assert(std::is_standard_layout_v<ConditionalVariable>, "ZHLN::ConditionalVariable must be standard layout!");
static_assert(std::is_trivially_default_constructible_v<ConditionalVariable>, "ZHLN::ConditionalVariable must be trivially default constructible!");
static_assert(std::is_trivially_copyable_v<ConditionalVariable>, "ZHLN::ConditionalVariable must be trivially copyable!");

}
