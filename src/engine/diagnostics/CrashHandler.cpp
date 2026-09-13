// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/diagnostics/CrashHandler.cpp
//
// Coordinates crash state: decides who dumps, when, and whether the process
// survives long enough to do it. This is what is left of AssertHandler.cpp once
// logging, stack traces, memory inspection and the sanitizer bridges have moved
// out, and it no longer knows what it is dumping.
//
// The old file included <Zahlen/Engine.hpp>, <Zahlen/Camera.hpp> and
// <Zahlen/physics/Physics.hpp> so it could reach engine->GetCamera().frustum
// and engine->GetPhysicsContext() directly. That made the crash handler depend
// on the whole engine plus Jolt, and meant adding a subsystem dump required
// editing the crash handler. Those dumps are now observers that Engine.cpp
// registers at startup (see CrashObservers.hpp); this file iterates them.
//
// Allocation: nothing on the crash path here allocates. Output goes through
// Diagnostics::WriteErr, which is a raw descriptor write; formatting is
// ZHLN::Format, which draws from a statically allocated pool rather than the
// heap; and the backtrace is captured into a stack buffer rather than a
// std::string. That matters because the allocator is a plausible thing to have
// broken, and malloc from inside a handler can take a lock the faulting thread
// already held.
//
// One coupling is deliberate and worth defending: tty/TTYBackend.hpp. It is a
// same-subsystem include, so it does not create the cross-subsystem dependency
// the observer bus exists to remove, and routing it through the registry would
// lose the restore when a signal arrives before Engine initialisation -- which
// is exactly when a half-configured terminal needs putting back.
//
// Note on what the subsystem observers are and are not: the bus itself is
// allocation-free, but an observer runs whatever dump function its subsystem
// registered, and ZHLN::Trace / ZHLN::Dump do allocate internally. Keeping those
// allocation-free means changing the reflection printers, which is out of scope
// here. That is also why the observers only run in the deferred path below.

#include "diagnostics/CrashObservers.hpp"
#include "diagnostics/DiagnosticsInternal.hpp"
#include "tty/TTYBackend.hpp"
#include <Zahlen/Core/Platform.hpp> // windows.h / unistd.h, HaltThread
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/Reflection.hpp> // Reflect::EnumToString
#include <Zahlen/Core/SignalManager.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/Thread.hpp> // GetCurrentFiberID
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib> // std::abort
#include <string_view>

namespace ZHLN {

namespace {

// ============================================================================
// Pending Crash State
// ============================================================================
// A worker thread that faults cannot dump safely, so it parks the event here and
// halts; the main thread picks it up in CheckForCrashes. All four are atomics
// because the writer is whichever thread died.

std::atomic<int>      s_PendingPhase {0}; // 0 = idle, 1 = crash parked, -1 = dumping
std::atomic<uint32_t> s_PendingKind {0};
std::atomic<void*>    s_FaultAddr {nullptr};
std::atomic<uint64_t> s_PendingThread {0};

// ============================================================================
// Crash Observer Registry
// ============================================================================
// Fixed array and an atomic count. Deliberately not a vector: this is walked
// from a signal handler, where a reallocation or a capacity change is a use
// after free, and where the registry being written concurrently with a walk is
// already the case the count guards against.

struct ObserverEntry {
    std::string_view           name {};
    Diagnostics::CrashObserver observer {nullptr};
    void*                      context {nullptr};
};

std::atomic<uint32_t> s_observerCount {0};
ObserverEntry         s_observers[Diagnostics::kMaxCrashObservers] {};

} // namespace

namespace Diagnostics {

auto RegisterCrashObserver(std::string_view name, CrashObserver observer, void* context) noexcept -> bool {
    if (observer == nullptr) {
        return false;
    }

    // load + store rather than fetch_add: registration is single-threaded by
    // contract (see CrashObservers.hpp), and this way a failed registration
    // leaves the count alone instead of burning a slot.
    const uint32_t index = s_observerCount.load(std::memory_order::relaxed);
    if (index >= kMaxCrashObservers) {
        return false;
    }

    s_observers[index] = ObserverEntry {.name = name, .observer = observer, .context = context};

    // Release so a handler on another thread that observes the new count also
    // observes the entry that was written before it.
    s_observerCount.store(index + 1, std::memory_order::release);
    return true;
}

void ClearCrashObservers() noexcept {
    // Drop the count first. A handler that starts after this sees zero entries
    // and skips the loop entirely, so the stale entries are never read.
    s_observerCount.store(0, std::memory_order::release);
}

void WriteCrashOutput(std::string_view text) noexcept {
    WriteErr(text);
}

} // namespace Diagnostics

namespace {

// Where the dump is running from, which decides both how much it may touch and
// how much stack it can spend on the backtrace.
enum class DumpContext : uint8_t {
    // Inside the signal handler on the main thread. The process is seconds from
    // _exit and the stack may be the thing that overflowed, so the budget stays
    // small and the subsystem observers are skipped: they walk live engine
    // state, which is exactly what a fault may have just corrupted. The old
    // code expressed this by passing engine == nullptr here; the enum says it
    // outright instead of relying on a null pointer to mean "don't look".
    InHandler,
    // On the main thread, after the faulting worker parked the event and halted.
    // Ordinary stack, scheduler still running, so this gets the full trace
    // depth and the subsystem dumps.
    DeferredMainThread,
};

// Runs the registered subsystem dumps. Writes a section header per observer so
// that a secondary fault inside one of them says which subsystem was mid-dump
// rather than leaving a truncated log with no attribution.
void RunCrashObservers(const SignalEvent& ev) noexcept {
    const uint32_t count = s_observerCount.load(std::memory_order::acquire);
    for (uint32_t i = 0; i < count && i < Diagnostics::kMaxCrashObservers; ++i) {
        const ObserverEntry& entry = s_observers[i];
        if (entry.observer == nullptr) {
            continue;
        }

        auto header = ZHLN::Format("\n{}--- {} STATE ---{}\n", Color::Cyan, entry.name, Color::Reset);
        Diagnostics::WriteErr(header.string_view());

        entry.observer(entry.context, ev);
    }
}

// Prints the backtrace section. `Capacity` doubles as the buffer size and the
// upper bound handed to the capture, so the two cannot drift apart.
template <size_t Capacity, int MaxFrames>
void WriteStackTrace() {
    char         stackBuf[Capacity] {};
    const size_t len = Diagnostics::CaptureStackTrace(stackBuf, Capacity, MaxFrames);
    if (len > 0) {
        Diagnostics::WriteErr(std::string_view(stackBuf, len));
    } else {
        Diagnostics::WriteErr("Not implemented\n");
    }
}

// `engineAlive` is the caller's assertion that there is an Engine worth walking.
// CheckForCrashes derives it from the pointer it is handed, exactly as the code
// this replaced gated its subsystem section on engine != nullptr.
void PerformDiagnosticDump(const SignalEvent& ev, DumpContext context, bool engineAlive) {
    const std::string_view sigName = Reflect::EnumToString(ev.signal);
    const void*            addr    = ev.faultAddress;

    auto sig_header  = ZHLN::Format("\n{}DIAGNOSTIC REPORT FOR SIGNAL: {}{}\n", Color::Red, sigName, Color::Reset);
    auto addr_header = ZHLN::Format("Faulting Address: {}{}{}\n", Color::Yellow, addr, Color::Reset);
    Diagnostics::WriteErr(sig_header.string_view());
    Diagnostics::WriteErr(addr_header.string_view());

    if (addr != nullptr) {
        Diagnostics::DumpFaultRegion(addr);
    }

    if (context == DumpContext::DeferredMainThread && engineAlive) {
        RunCrashObservers(ev);
    }

    Diagnostics::WriteErr("\nStack Trace:\n");

    // Captured into the stack rather than returned as a std::string. Templated
    // on the budget rather than taking it as an argument because the buffer has
    // to be sized at compile time -- a runtime capacity would mean sizing the
    // array for the worst case, and reserving the deferred path's 16 KB inside
    // the signal handler is the thing this is trying to avoid.
    if (context == DumpContext::InHandler) {
        WriteStackTrace<4096, 48>();
    } else {
        WriteStackTrace<16384, 128>();
    }
    Diagnostics::WriteErr("\n");
}

void ProcessCrash(const SignalEvent& ev) {
    // Panic already printed the stacktrace and then aborted.
    if (ev.signal == Signal::Abort) {
        _exit(1);
    }

    int expected = 0;
    if (!s_PendingPhase.compare_exchange_strong(expected, 1)) {
        if (expected == -1) {
            ZHLN::Println("!! SECONDARY CRASH DURING DIAGNOSTICS. ABORTING !!");
        }
        _exit(1);
    }

    s_PendingKind.store(static_cast<uint32_t>(ev.signal), std::memory_order::relaxed);
    s_FaultAddr.store(ev.faultAddress, std::memory_order::relaxed);
    s_PendingThread.store(ev.threadId, std::memory_order::relaxed);

    if (ZHLN::GetCurrentFiberID() == 1) {
        ZHLN::Print("\n[ZHLN] Terminal signal on Main Thread. Attempting emergency dump...\n");

        s_PendingPhase.store(-1);
        TTYBackend::EmergencyRestore();
        PerformDiagnosticDump(ev, DumpContext::InHandler, false);
        _exit(1);
    }

    ZHLN::Print("\n[ZHLN] Signal intercepted in Worker. Main Thread will dump soon...\n");
    while (true) {
        HaltThread();
    }
}

struct HandleRequestStop {
    ZHLN_ANNOTATION(ZHLN::SignalSafe {})
    void operator()(const SignalEvent&) const noexcept {
        TTYBackend::EmergencyRestore();
        _exit(0);
    }
};

struct HandleAbort {
    ZHLN_ANNOTATION(ZHLN::SignalSafe {})
    void operator()(const SignalEvent&) const noexcept {
        _exit(1);
    }
};

struct HandleFatal {
    ZHLN_ANNOTATION(ZHLN::SignalSafe {})
    void operator()(const SignalEvent& ev) const noexcept {
        ProcessCrash(ev);
    }
};

} // namespace

void CheckForCrashes(Engine* engine) {
    if (s_PendingPhase.load(std::memory_order::acquire) != 1) {
        return;
    }

    // The engine pointer is no longer threaded down to the dump -- the
    // subsystems it stood for describe themselves through the observer bus --
    // but a null engine still means "there is nothing to walk", so it gates the
    // observer pass the same way it gated the hardcoded dumps before.
    const bool engineAlive = (engine != nullptr);

    s_PendingPhase.store(-1, std::memory_order::release);
    const SignalEvent ev {
        .signal       = static_cast<Signal>(s_PendingKind.load(std::memory_order::relaxed)),
        .faultAddress = s_FaultAddr.load(std::memory_order::relaxed),
        .threadId     = s_PendingThread.load(std::memory_order::relaxed),
    };
    PerformDiagnosticDump(ev, DumpContext::DeferredMainThread, engineAlive);
    std::abort();
}

void SetupSignalHandler() {
    static std::atomic<bool> s_defaultsRegistered {false};
    if (!s_defaultsRegistered.exchange(true, std::memory_order::acq_rel)) {
        SignalManager::RegisterSafeHandler<HandleRequestStop {}>(Signal::Interrupt);
        SignalManager::RegisterSafeHandler<HandleRequestStop {}>(Signal::Terminate);
        SignalManager::RegisterSafeHandler<HandleFatal {}>(Signal::AccessViolation);
        SignalManager::RegisterSafeHandler<HandleFatal {}>(Signal::IllegalInstruction);
        SignalManager::RegisterSafeHandler<HandleFatal {}>(Signal::MathError);
        SignalManager::RegisterSafeHandler<HandleFatal {}>(Signal::BusError);
        SignalManager::RegisterSafeHandler<HandleAbort {}>(Signal::Abort);
    }
    // Warm the symbolizer now, while the process is healthy, so the first stack
    // trace is not also the first DbgHelp initialisation.
    Diagnostics::InitializeSymbolResolver();
    SignalManager::Install();
}

} // namespace ZHLN
