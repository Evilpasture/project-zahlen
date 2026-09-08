// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/SignalSafe.hpp
//
// Portable signal vocabulary. Application code annotates handlers with
// SignalSafe and talks in Signal / SignalEvent; POSIX <csignal> and Windows
// SEH stay in the manager implementation.

#pragma once

#include <cstdint>

namespace ZHLN {

/// Annotation tag: this callable is safe to run from a signal / VEH context.
struct SignalSafe {};

enum class Signal : uint32_t {
    Interrupt,           // SIGINT / Ctrl+C
    Terminate,           // SIGTERM / console close
    Quit,                // SIGQUIT (POSIX)
    User1,               // SIGUSR1 (POSIX)
    User2,               // SIGUSR2 (POSIX)
    AccessViolation,     // SIGSEGV / EXCEPTION_ACCESS_VIOLATION
    IllegalInstruction,  // SIGILL
    MathError,           // SIGFPE
    BusError,            // SIGBUS (POSIX)
    Abort,               // SIGABRT (panic already dumped)
};

struct SignalEvent {
    Signal   signal        = Signal::Interrupt;
    void*    faultAddress  = nullptr;
    uint64_t threadId      = 0;
};

} // namespace ZHLN
