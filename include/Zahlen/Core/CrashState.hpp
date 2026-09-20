// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/CrashState.hpp
//
// The state a crash dump needs, as a value the caller owns.
//
// This used to be six file-scope statics inside diagnostics/CrashHandler.cpp:
// four atomics holding the parked crash, plus an observer registry. Hidden
// mutable state at namespace scope is bad anywhere, but here it had concrete
// costs -- nothing could say who owned the crash state, a test could not run
// two dumps without them interfering, and a second engine in the same process
// would have silently shared the first one's pending-crash slot.
//
// So the caller declares one and hands it to SetupSignalHandler:
//
//     static ZHLN::CrashState g_crashState;
//     ZHLN::SetupSignalHandler(g_crashState);
//
// Static storage duration is a requirement, not a suggestion: the address is
// copied into the signal handler slots at registration, and a handler that
// fires after the state is gone writes to freed memory. A function-local static
// in main() or a namespace-scope static both satisfy this.
//
// Every member is constant-initialised, so a `static CrashState` is ready before
// any dynamic initialisation runs and there is no init-order trap. That matters
// because a fault during static initialisation is exactly when this is needed.

#pragma once

#include <Zahlen/Core/SignalSafe.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ZHLN {

/// Dumps one subsystem's state during a crash.
///
/// `context` is whatever was passed to RegisterCrashObserver, which is how a
/// member function gets here: register a captureless lambda that casts `context`
/// back to the subsystem.
///
/// Called from a signal / VEH context. Assume the process is already dying: take
/// no locks the faulting thread may hold, avoid allocating, never throw.
using CrashObserver = void (*)(void* context, const SignalEvent& event) noexcept;

/// Room for eight subsystems. Fixed, because a std::vector here would allocate
/// inside the thing that runs when the allocator may be the thing that broke.
inline constexpr size_t kMaxCrashObservers = 8;

/// One registered subsystem dump.
///
/// `name` does not own its characters; it must outlive the registration. Every
/// registrant passes a string literal.
struct CrashObserverEntry {
    std::string_view name {};
    CrashObserver    observer {nullptr};
    void*            context {nullptr};
};

/// Everything a crash dump touches. Caller-owned; see the file header.
struct CrashState {
    // --- The parked crash
    // A worker thread that faults cannot dump safely, so it records the event
    // here and halts; the main thread picks it up in CheckForCrashes. All
    // atomics because the writer is whichever thread died.

    /// 0 = idle, 1 = a crash is parked waiting for the main thread,
    /// -1 = a dump is in progress (so a second fault is reported, not followed).
    std::atomic<int>      pendingPhase {0};
    std::atomic<uint32_t> pendingKind {0};
    std::atomic<void*>    faultAddr {nullptr};
    std::atomic<uint64_t> pendingThread {0};

    /// Makes SetupSignalHandler idempotent per state, replacing a file-scope
    /// guard. Registering twice would install duplicate handlers and print every
    /// crash dump twice.
    std::atomic<bool> handlersRegistered {false};

    // --- Subsystem dump registry
    // Fixed array plus an atomic count. Deliberately not a vector: this is
    // walked from a signal handler, where a reallocation would be a use after
    // free.

    std::atomic<uint32_t> observerCount {0};
    CrashObserverEntry    observers[kMaxCrashObservers] {};
};

} // namespace ZHLN
