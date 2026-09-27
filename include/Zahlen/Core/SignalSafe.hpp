// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <cstdint>

namespace ZHLN {

struct SignalSafe {};

enum class Signal : uint32_t {
    Interrupt,
    Terminate,
    Quit,
    User1,
    User2,
    AccessViolation,
    IllegalInstruction,
    MathError,
    BusError,
    Abort,
};

struct SignalEvent {
    Signal   signal        = Signal::Interrupt;
    void*    faultAddress  = nullptr;
    uint64_t threadId      = 0;
};

}
