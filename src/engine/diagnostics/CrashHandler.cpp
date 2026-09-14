// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/diagnostics/CrashHandler.cpp
//
// Coordinates crash state: decides who dumps, when, and whether the process
// survives long enough to do it. This is what is left of AssertHandler.cpp once
// logging, stack traces, memory inspection and the sanitizer bridges have moved
// out, and it no longer knows what it is dumping.
//
// Two things this file deliberately does not own:
//
// WHAT to dump. The old file included <Zahlen/Engine.hpp>, <Zahlen/Camera.hpp>
// and <Zahlen/physics/Physics.hpp> to reach engine->GetCamera().frustum and
// engine->GetPhysicsContext() directly, so the crash path depended on the whole
// engine plus Jolt and adding a subsystem dump meant editing the crash handler.
// Those dumps are now observers that Engine.cpp registers at startup (see
// CrashObservers.hpp); this file iterates them.
//
// WHERE the state lives. It used to be six file-scope statics here. They are now
// a CrashState the caller declares and passes to SetupSignalHandler, so
// ownership is visible at the call site, two dumps cannot interfere, and a test
// can drive this code directly. The pointer reaches the signal handlers through
// SignalManager's stateful slots -- RegisterHandler copies the functor into the
// slot and hands it back on dispatch -- so the signal path has no global to look
// up either.
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
#include <Zahlen/Core/CrashState.hpp>
#include <Zahlen/Core/Platform.hpp> // windows.h / unistd.h, HaltThread
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/Reflection.hpp> // Reflect::EnumToString
#include <Zahlen/Core/SignalManager.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/Thread.hpp> // GetCurrentFiberID
#include <cstddef>
#include <cstdint>
#include <cstdlib> // std::abort
#include <string_view>

namespace ZHLN {

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
void RunCrashObservers(CrashState& state, const SignalEvent& ev) noexcept {
    const uint32_t count = state.observerCount.load(std::memory_order::acquire);
    for (uint32_t i = 0; i < count && i < kMaxCrashObservers; ++i) {
        const CrashObserverEntry& entry = state.observers[i];
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
    const size_t len = Diagnostics::CaptureStackTrace(stackBuf, MaxFrames);
    if (len > 0) {
        Diagnostics::WriteErr(std::string_view(stackBuf, len));
    } else {
        Diagnostics::WriteErr("Not implemented\n");
    }
}

// `engineAlive` is the caller's assertion that there is an Engine worth walking.
// CheckForCrashes derives it from the pointer it is handed, exactly as the code
// this replaced gated its subsystem section on engine != nullptr.
void PerformDiagnosticDump(CrashState& state, const SignalEvent& ev, DumpContext context, bool engineAlive) {
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
        RunCrashObservers(state, ev);
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

void ProcessCrash(CrashState& state, const SignalEvent& ev) {
    // Panic already printed the stacktrace and then aborted.
    if (ev.signal == Signal::Abort) {
        _exit(1);
    }

    int expected = 0;
    if (!state.pendingPhase.compare_exchange_strong(expected, 1)) {
        if (expected == -1) {
            ZHLN::Println("!! SECONDARY CRASH DURING DIAGNOSTICS. ABORTING !!");
        }
        _exit(1);
    }

    state.pendingKind.store(static_cast<uint32_t>(ev.signal), std::memory_order::relaxed);
    state.faultAddr.store(ev.faultAddress, std::memory_order::relaxed);
    state.pendingThread.store(ev.threadId, std::memory_order::relaxed);

    if (ZHLN::GetCurrentFiberID() == 1) {
        ZHLN::Print("\n[ZHLN] Terminal signal on Main Thread. Attempting emergency dump...\n");

        state.pendingPhase.store(-1);
        TTYBackend::EmergencyRestore();
        PerformDiagnosticDump(state, ev, DumpContext::InHandler, false);
        _exit(1);
    }

    ZHLN::Print("\n[ZHLN] Signal intercepted in Worker. Main Thread will dump soon...\n");
    while (true) {
        HaltThread();
    }
}

// Handlers are trivially copyable functors of one pointer, which is what lets
// SignalManager store them in a slot and hand the pointer back on dispatch. That
// is how the caller-owned CrashState reaches a signal handler with no global
// anywhere on the path.

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
    CrashState* state = nullptr;

    ZHLN_ANNOTATION(ZHLN::SignalSafe {})
    void operator()(const SignalEvent& ev) const noexcept {
        ProcessCrash(*state, ev);
    }
};

} // namespace

namespace Diagnostics {

auto RegisterCrashObserver(CrashState& state, std::string_view name, CrashObserver observer, void* context) noexcept -> bool {
    if (observer == nullptr) {
        return false;
    }

    // load + store rather than fetch_add: registration is single-threaded by
    // contract (see CrashObservers.hpp), and this way a failed registration
    // leaves the count alone instead of burning a slot.
    const uint32_t index = state.observerCount.load(std::memory_order::relaxed);
    if (index >= kMaxCrashObservers) {
        return false;
    }

    state.observers[index] = CrashObserverEntry {.name = name, .observer = observer, .context = context};

    // Release so a handler on another thread that observes the new count also
    // observes the entry that was written before it.
    state.observerCount.store(index + 1, std::memory_order::release);
    return true;
}

void ClearCrashObservers(CrashState& state) noexcept {
    // Drop the count first. A handler that starts after this sees zero entries
    // and skips the loop entirely, so the stale entries are never read.
    state.observerCount.store(0, std::memory_order::release);
}

void WriteCrashOutput(std::string_view text) noexcept {
    WriteErr(text);
}

} // namespace Diagnostics

void CheckForCrashes(CrashState& state, Engine* engine) {
    if (state.pendingPhase.load(std::memory_order::acquire) != 1) {
        return;
    }

    // The engine pointer is no longer threaded down to the dump -- the
    // subsystems it stood for describe themselves through the observer bus --
    // but a null engine still means "there is nothing to walk", so it gates the
    // observer pass the same way it gated the hardcoded dumps before.
    const bool engineAlive = (engine != nullptr);

    state.pendingPhase.store(-1, std::memory_order::release);
    const SignalEvent ev {
        .signal       = static_cast<Signal>(state.pendingKind.load(std::memory_order::relaxed)),
        .faultAddress = state.faultAddr.load(std::memory_order::relaxed),
        .threadId     = state.pendingThread.load(std::memory_order::relaxed),
    };
    PerformDiagnosticDump(state, ev, DumpContext::DeferredMainThread, engineAlive);
    std::abort();
}

void SetupSignalHandler(CrashState& state) {
    // Idempotent per state rather than per process: the guard used to be a
    // file-scope static, which meant two CrashStates in one process silently
    // shared one registration and the second got no handlers at all.
    if (!state.handlersRegistered.exchange(true, std::memory_order::acq_rel)) {
        SignalManager::RegisterHandler(Signal::Interrupt, HandleRequestStop {});
        SignalManager::RegisterHandler(Signal::Terminate, HandleRequestStop {});
        SignalManager::RegisterHandler(Signal::AccessViolation, HandleFatal {.state = &state});
        SignalManager::RegisterHandler(Signal::IllegalInstruction, HandleFatal {.state = &state});
        SignalManager::RegisterHandler(Signal::MathError, HandleFatal {.state = &state});
        SignalManager::RegisterHandler(Signal::BusError, HandleFatal {.state = &state});
        SignalManager::RegisterHandler(Signal::Abort, HandleAbort {});
    }
    // Warm the symbolizer now, while the process is healthy, so the first stack
    // trace is not also the first DbgHelp initialisation.
    Diagnostics::InitializeSymbolResolver();
    SignalManager::Install();
}

} // namespace ZHLN
